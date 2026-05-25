#include <uv.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "common/log.hpp"
#include "common/uv_check.hpp"

namespace {

consteval int         kDefaultPort() { return 7000; }
constexpr const char* kDefaultHost = "127.0.0.1";

struct WriteReq {
    uv_write_t req{};
    uv_buf_t   buf{};
};

// =========================================================================
//  Stdin 모드
// =========================================================================

namespace stdin_mode {

uv_tcp_t  g_socket{};
uv_tty_t  g_stdin_tty{};
uv_pipe_t g_stdin_pipe{};
bool      g_stdin_is_tty = false;

uv_stream_t* server_stream() {
    return reinterpret_cast<uv_stream_t*>(&g_socket);
}

uv_stream_t* stdin_stream() {
    return g_stdin_is_tty
        ? reinterpret_cast<uv_stream_t*>(&g_stdin_tty)
        : reinterpret_cast<uv_stream_t*>(&g_stdin_pipe);
}

void shutdown_loop(uv_loop_t* loop) noexcept {
    uv_walk(loop,
            +[](uv_handle_t* h, void*) noexcept {
                if (false == uv_is_closing(h)) {
                    uv_close(h, nullptr);
                }
            },
            nullptr);
}

void on_alloc(uv_handle_t* /*h*/, std::size_t suggested, uv_buf_t* buf) noexcept {
    buf->base = new char[suggested];
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_write(uv_write_t* req, int status) noexcept {
    auto* w = reinterpret_cast<WriteReq*>(req);
    if (status < 0) {
        uvx::log::error("write error: {}", uv_strerror(status));
    }
    delete[] w->buf.base;
    delete w;
}

void on_server_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) noexcept {
    if (nread > 0) {
        std::cout.write(buf->base, nread);
        std::cout.flush();
    } else if (nread < 0) {
        if (UV_EOF != nread) {
            uvx::log::warn("server read error: {}",
                           uv_strerror(static_cast<int>(nread)));
        } else {
            uvx::log::info("server closed connection");
        }
        shutdown_loop(stream->loop);
    }
    if (nullptr != buf->base && nread <= 0) {
        delete[] buf->base;
    }
}

void on_stdin_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) noexcept {
    if (nread > 0) {
        auto*     w  = new WriteReq{};
        w->buf       = uv_buf_init(buf->base, static_cast<unsigned int>(nread));
        const int rc = uv_write(&w->req, server_stream(), &w->buf, 1, on_write);
        if (rc < 0) {
            uvx::log::error("uv_write submit failed: {}", uv_strerror(rc));
            delete[] w->buf.base;
            delete w;
        }
        return;
    }
    if (nread < 0) {
        if (UV_EOF != nread) {
            uvx::log::warn("stdin read error: {}",
                           uv_strerror(static_cast<int>(nread)));
        } else {
            uvx::log::info("stdin EOF, sending shutdown to server");
        }
        uv_read_stop(stream);
        if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(stream))) {
            uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
        }
        static uv_shutdown_t shutdown_req{};
        uv_shutdown(&shutdown_req, server_stream(),
                    +[](uv_shutdown_t*, int status) noexcept {
                        if (status < 0) {
                            uvx::log::warn("shutdown failed: {}", uv_strerror(status));
                        }
                    });
    }
    if (nullptr != buf->base && nread <= 0) {
        delete[] buf->base;
    }
}

void start_stdin_reading(uv_loop_t* loop) {
    const uv_handle_type t = uv_guess_handle(0);
    if (UV_TTY == t) {
        uvx::check(uv_tty_init(loop, &g_stdin_tty, 0, /*readable=*/1), "uv_tty_init");
        g_stdin_is_tty = true;
    } else {
        uvx::check(uv_pipe_init(loop, &g_stdin_pipe, 0), "uv_pipe_init");
        uvx::check(uv_pipe_open(&g_stdin_pipe, 0), "uv_pipe_open");
        g_stdin_is_tty = false;
    }
    uvx::check(uv_read_start(stdin_stream(), on_alloc, on_stdin_read),
               "uv_read_start(stdin)");
}

void on_connect(uv_connect_t* req, int status) noexcept {
    if (status < 0) {
        uvx::log::error("connect failed: {}", uv_strerror(status));
        shutdown_loop(req->handle->loop);
        return;
    }
    uvx::log::info("connected. type a line and press Enter "
                   "(Ctrl+Z then Enter to send EOF on Windows)");

    if (uv_read_start(req->handle, on_alloc, on_server_read) < 0) {
        uvx::log::error("server read start failed");
        shutdown_loop(req->handle->loop);
        return;
    }
    try {
        start_stdin_reading(req->handle->loop);
    } catch (const std::exception& e) {
        uvx::log::error("stdin init failed: {}", e.what());
        shutdown_loop(req->handle->loop);
    }
}

int run(const char* host, int port) {
    uv_loop_t* loop = uv_default_loop();

    uvx::check(uv_tcp_init(loop, &g_socket), "uv_tcp_init");

    sockaddr_in dest{};
    uvx::check(uv_ip4_addr(host, port, &dest), "uv_ip4_addr");

    uv_connect_t connect_req{};
    uvx::check(uv_tcp_connect(&connect_req, &g_socket,
                              reinterpret_cast<const sockaddr*>(&dest),
                              on_connect),
               "uv_tcp_connect");

    uvx::log::info("connecting to {}:{} ...", host, port);

    const int rc = uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
    return rc;
}

}  // namespace stdin_mode

// =========================================================================
//  Multi-conn 모드
// =========================================================================

namespace multi_mode {

struct MultiCtx;

struct ClientConn {
    int          conn_id = 0;
    uv_tcp_t     socket{};
    uv_connect_t connect_req{};
    std::string  ping;
    bool         got_reply = false;
    MultiCtx*    ctx       = nullptr;
};

struct MultiCtx {
    uv_loop_t                  loop{};
    std::vector<ClientConn*>   conns;
    int                        connected = 0;
    int                        finished  = 0;
    int                        total     = 0;
};

void on_alloc(uv_handle_t* /*h*/, std::size_t suggested, uv_buf_t* buf) noexcept {
    buf->base = new char[suggested];
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_close_conn(uv_handle_t* handle) noexcept {
    auto* sock = reinterpret_cast<uv_tcp_t*>(handle);
    auto* conn = reinterpret_cast<ClientConn*>(sock->data);
    auto* ctx  = conn->ctx;

    ctx->finished++;
    if (ctx->finished >= ctx->total) {
        uvx::log::info("multi-mode: all {} connections finished", ctx->total);
    }
}

void on_write(uv_write_t* req, int status) noexcept {
    auto* w = reinterpret_cast<WriteReq*>(req);
    if (status < 0) {
        uvx::log::error("multi-mode write error: {}", uv_strerror(status));
    }
    delete[] w->buf.base;
    delete w;
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) noexcept {
    auto* sock = reinterpret_cast<uv_tcp_t*>(stream);
    auto* conn = reinterpret_cast<ClientConn*>(sock->data);

    if (nread > 0) {
        if (false == conn->got_reply) {
            conn->got_reply = true;
            const std::string recv_str(buf->base, static_cast<std::size_t>(nread));
            uvx::log::info("conn {:3} echo received ({}B): {}",
                           conn->conn_id, nread,
                           recv_str.substr(0, recv_str.size() - 1));
            if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(sock))) {
                uv_close(reinterpret_cast<uv_handle_t*>(sock), on_close_conn);
            }
        }
    } else if (nread < 0) {
        if (UV_EOF != nread) {
            uvx::log::warn("conn {} read error: {}",
                           conn->conn_id, uv_strerror(static_cast<int>(nread)));
        }
        if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(sock))) {
            uv_close(reinterpret_cast<uv_handle_t*>(sock), on_close_conn);
        }
    }

    if (nullptr != buf->base) {
        delete[] buf->base;
    }
}

void on_connect(uv_connect_t* req, int status) noexcept {
    auto* sock = reinterpret_cast<uv_tcp_t*>(req->handle);
    auto* conn = reinterpret_cast<ClientConn*>(sock->data);

    if (status < 0) {
        uvx::log::error("conn {} connect failed: {}",
                        conn->conn_id, uv_strerror(status));
        // socket close — loop 가 hang 하지 않도록 handle 정리.
        // on_close_conn 이 ctx->finished++ 처리하므로 여기서 직접 ++ 하지 않음.
        if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(sock))) {
            uv_close(reinterpret_cast<uv_handle_t*>(sock), on_close_conn);
        }
        return;
    }

    conn->ctx->connected++;

    auto*    w   = new WriteReq{};
    auto*    buf = new char[conn->ping.size()];
    std::memcpy(buf, conn->ping.data(), conn->ping.size());
    w->buf       = uv_buf_init(buf, static_cast<unsigned int>(conn->ping.size()));

    const int rc = uv_write(&w->req,
                            reinterpret_cast<uv_stream_t*>(sock),
                            &w->buf, 1, on_write);
    if (rc < 0) {
        uvx::log::error("conn {} write submit failed: {}", conn->conn_id, uv_strerror(rc));
        delete[] w->buf.base;
        delete w;
    }

    uv_read_start(reinterpret_cast<uv_stream_t*>(sock), on_alloc, on_read);
}

int run(const char* host, int port, int conn_count) {
    MultiCtx ctx;
    ctx.total = conn_count;
    uv_loop_init(&ctx.loop);

    sockaddr_in dest{};
    uvx::check(uv_ip4_addr(host, port, &dest), "uv_ip4_addr");

    ctx.conns.reserve(conn_count);
    for (int i = 0; i < conn_count; ++i) {
        auto* conn    = new ClientConn{};
        conn->conn_id = i;
        conn->ctx     = &ctx;
        conn->ping    = "ping-from-" + std::to_string(i) + "\n";

        uvx::check(uv_tcp_init(&ctx.loop, &conn->socket), "uv_tcp_init");
        conn->socket.data = conn;

        uvx::check(uv_tcp_connect(&conn->connect_req, &conn->socket,
                                  reinterpret_cast<const sockaddr*>(&dest),
                                  on_connect),
                   "uv_tcp_connect");

        ctx.conns.emplace_back(conn);
    }

    uvx::log::info("multi-mode: connecting {} clients to {}:{}", conn_count, host, port);

    const int rc = uv_run(&ctx.loop, UV_RUN_DEFAULT);
    uv_loop_close(&ctx.loop);

    for (auto* c : ctx.conns) {
        delete c;
    }

    uvx::log::info("multi-mode: done (connected={}, finished={}/{})",
                   ctx.connected, ctx.finished, ctx.total);
    return rc;
}

}  // namespace multi_mode

}  // namespace

int main(int argc, char* argv[]) {
    uvx::config::Config cfg;
    cfg.load_first_existing({
        "uvx.yaml",
        "config/uvx.yaml",
        "../uvx.yaml",
        "../config/uvx.yaml",
        "../../uvx.yaml",
        "../../config/uvx.yaml",
        "../../../config/uvx.yaml",
    });
    if (cfg.loaded()) {
        uvx::log::info("config loaded: {}", cfg.path());
    }

    // env > yaml > default (cli argv 가 최우선)
    const std::string default_host = uvx::config::env_or_cfg_str(cfg, nullptr,
                                                                  "client.host", kDefaultHost);
    const int default_port = uvx::config::env_or_cfg_int(cfg, nullptr,
                                                          "client.port", kDefaultPort());
    const int default_conns = uvx::config::env_or_cfg_int(cfg, "ECHO_CLIENT_CONNS",
                                                           "client.conns", 0);

    const std::string host_str = (argc >= 2) ? std::string{argv[1]} : default_host;
    const int         port     = (argc >= 3) ? std::atoi(argv[2])   : default_port;

    if (default_conns > 0) {
        return multi_mode::run(host_str.c_str(), port, default_conns);
    }
    return stdin_mode::run(host_str.c_str(), port);
}
