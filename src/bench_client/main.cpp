#include <uv.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "uvx/core/config.hpp"
#include "uvx/core/log.hpp"
#include "uvx/core/uv_check.hpp"

namespace {

constexpr const char* kDefaultHost = "127.0.0.1";
consteval int         kDefaultPort()        { return 7000; }
consteval int         kDefaultConns()       { return 16; }
consteval int         kDefaultDurationSec() { return 10; }
consteval int         kDefaultMsgSize()     { return 64; }

struct BenchConn;

struct BenchCtx {
    uv_loop_t               loop{};
    uv_timer_t              deadline{};
    std::vector<BenchConn*> conns;
    int                     conn_count    = 0;
    int                     duration_sec  = 0;
    int                     msg_size      = 0;
    uint64_t                start_time_ns = 0;
    bool                    shutting_down = false;
    int                     active_conns  = 0;
};

struct BenchConn {
    int               conn_id = 0;
    uv_tcp_t          socket{};
    uv_connect_t      connect_req{};
    uv_write_t        write_req{};
    uv_buf_t          write_buf{};
    std::vector<char> send_data;
    std::size_t       recv_remaining = 0;
    uint64_t          send_time_ns   = 0;

    uint64_t          rtt_sum_ns = 0;
    uint64_t          rtt_min_ns = std::numeric_limits<uint64_t>::max();
    uint64_t          rtt_max_ns = 0;
    uint64_t          req_count  = 0;
    uint64_t          bytes_sent = 0;
    uint64_t          bytes_recv = 0;

    bool              connected = false;
    BenchCtx*         ctx       = nullptr;
};

void on_alloc(uv_handle_t* /*h*/, std::size_t suggested, uv_buf_t* buf) noexcept {
    buf->base = new char[suggested];
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_close_conn(uv_handle_t* handle) noexcept {
    auto* sock = reinterpret_cast<uv_tcp_t*>(handle);
    auto* conn = reinterpret_cast<BenchConn*>(sock->data);
    auto* ctx  = conn->ctx;

    ctx->active_conns--;
    if (ctx->active_conns <= 0) {
        if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(&ctx->deadline))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&ctx->deadline), nullptr);
        }
    }
}

void close_conn(BenchConn* conn) noexcept {
    if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(&conn->socket))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&conn->socket), on_close_conn);
    }
}

void send_next(BenchConn* conn) noexcept;

void on_write(uv_write_t* /*req*/, int status) noexcept {
    if (status < 0) {
        uvx::log::warn("write error: {}", uv_strerror(status));
    }
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) noexcept {
    auto* sock = reinterpret_cast<uv_tcp_t*>(stream);
    auto* conn = reinterpret_cast<BenchConn*>(sock->data);
    auto* ctx  = conn->ctx;

    if (nread < 0) {
        if (UV_EOF != nread && false == ctx->shutting_down) {
            uvx::log::warn("conn {} read error: {}",
                           conn->conn_id, uv_strerror(static_cast<int>(nread)));
        }
        close_conn(conn);
        if (nullptr != buf->base) delete[] buf->base;
        return;
    }

    if (nread > 0) {
        conn->bytes_recv += static_cast<uint64_t>(nread);

        if (conn->recv_remaining >= static_cast<std::size_t>(nread)) {
            conn->recv_remaining -= static_cast<std::size_t>(nread);
        } else {
            conn->recv_remaining = 0;
        }

        while (0 == conn->recv_remaining) {
            const uint64_t now_ns = uv_hrtime();
            const uint64_t rtt    = now_ns - conn->send_time_ns;

            conn->rtt_sum_ns += rtt;
            if (rtt < conn->rtt_min_ns) conn->rtt_min_ns = rtt;
            if (rtt > conn->rtt_max_ns) conn->rtt_max_ns = rtt;
            conn->req_count++;

            if (ctx->shutting_down) {
                close_conn(conn);
                break;
            }

            send_next(conn);
            break;
        }
    }

    if (nullptr != buf->base) delete[] buf->base;
}

void send_next(BenchConn* conn) noexcept {
    conn->send_time_ns   = uv_hrtime();
    conn->recv_remaining = conn->send_data.size();
    conn->write_buf      = uv_buf_init(conn->send_data.data(),
                                       static_cast<unsigned int>(conn->send_data.size()));
    conn->bytes_sent    += conn->send_data.size();

    const int rc = uv_write(&conn->write_req,
                            reinterpret_cast<uv_stream_t*>(&conn->socket),
                            &conn->write_buf, 1, on_write);
    if (rc < 0) {
        uvx::log::warn("conn {} write submit failed: {}", conn->conn_id, uv_strerror(rc));
        close_conn(conn);
    }
}

void on_connect(uv_connect_t* req, int status) noexcept {
    auto* sock = reinterpret_cast<uv_tcp_t*>(req->handle);
    auto* conn = reinterpret_cast<BenchConn*>(sock->data);

    if (status < 0) {
        uvx::log::error("conn {} connect failed: {}", conn->conn_id, uv_strerror(status));
        close_conn(conn);
        return;
    }

    conn->connected = true;
    conn->send_data.assign(static_cast<std::size_t>(conn->ctx->msg_size), 'A');
    conn->send_data.back() = '\n';

    uvx::check(uv_read_start(reinterpret_cast<uv_stream_t*>(sock), on_alloc, on_read),
               "uv_read_start");

    send_next(conn);
}

void on_deadline(uv_timer_t* timer) noexcept {
    auto* ctx = reinterpret_cast<BenchCtx*>(timer->data);
    uvx::log::info("deadline reached, draining {} connection(s)...",
                   ctx->active_conns);
    ctx->shutting_down = true;

    for (auto* c : ctx->conns) {
        close_conn(c);
    }
}

void print_results(const BenchCtx& ctx) noexcept {
    const uint64_t end_ns      = uv_hrtime();
    const double   elapsed_sec = static_cast<double>(end_ns - ctx.start_time_ns) / 1e9;

    uint64_t total_req    = 0;
    uint64_t total_sent   = 0;
    uint64_t total_recv   = 0;
    uint64_t rtt_sum      = 0;
    uint64_t rtt_min      = std::numeric_limits<uint64_t>::max();
    uint64_t rtt_max      = 0;

    for (const auto* c : ctx.conns) {
        total_req  += c->req_count;
        total_sent += c->bytes_sent;
        total_recv += c->bytes_recv;
        rtt_sum    += c->rtt_sum_ns;
        if (c->rtt_min_ns < rtt_min) rtt_min = c->rtt_min_ns;
        if (c->rtt_max_ns > rtt_max) rtt_max = c->rtt_max_ns;
    }

    const double rps        = (elapsed_sec > 0) ? static_cast<double>(total_req) / elapsed_sec : 0.0;
    const double mb_per_sec = (elapsed_sec > 0)
        ? static_cast<double>(total_sent + total_recv) / elapsed_sec / (1024.0 * 1024.0)
        : 0.0;
    const double rtt_avg_us = (total_req > 0) ? static_cast<double>(rtt_sum) / total_req / 1000.0 : 0.0;
    const double rtt_min_us = (rtt_min != std::numeric_limits<uint64_t>::max())
        ? static_cast<double>(rtt_min) / 1000.0 : 0.0;
    const double rtt_max_us = static_cast<double>(rtt_max) / 1000.0;

    uvx::log::info("===== Benchmark Result =====");
    uvx::log::info("  duration:     {:.2f} sec", elapsed_sec);
    uvx::log::info("  connections:  {}", ctx.conn_count);
    uvx::log::info("  message size: {} bytes", ctx.msg_size);
    uvx::log::info("  total req:    {}", total_req);
    uvx::log::info("  throughput:   {:.0f} req/s   {:.2f} MB/s (tx+rx)", rps, mb_per_sec);
    uvx::log::info("  RTT avg:      {:.1f} us  min: {:.1f} us  max: {:.1f} us",
                   rtt_avg_us, rtt_min_us, rtt_max_us);
    uvx::log::info("============================");
}

// 호스트명을 IPv4 sockaddr 로 resolve. numeric IP 면 즉시 변환, 아니면 DNS lookup 수행.
bool resolve_host(uv_loop_t* loop, const std::string& host, int port, sockaddr_in* out) noexcept {
    if (port < 1 || port > 65535) {
        uvx::log::error("invalid port {} (must be 1-65535)", port);
        return false;
    }

    // 1) numeric IP fast path
    if (0 == uv_ip4_addr(host.c_str(), port, out)) {
        return true;
    }

    // 2) DNS lookup (callback nullptr → 동기 실행)
    uv_getaddrinfo_t req{};
    addrinfo         hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    const int rc = uv_getaddrinfo(loop, &req, nullptr,
                                  host.c_str(),
                                  std::to_string(port).c_str(),
                                  &hints);
    if (0 != rc || nullptr == req.addrinfo) {
        uvx::log::error("uv_getaddrinfo({}) failed: {}", host, uv_strerror(rc));
        return false;
    }

    std::memcpy(out, req.addrinfo->ai_addr, sizeof(sockaddr_in));
    // service name 으로 받은 port 가 비어있을 가능성 대비, 명시적으로 다시 set
    out->sin_port = htons(static_cast<uint16_t>(port));
    uv_freeaddrinfo(req.addrinfo);
    return true;
}

}  // namespace

int main() try {
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

    BenchCtx ctx;
    const std::string host = uvx::config::env_or_cfg_str(cfg, "BENCH_HOST",
                                                          "bench.host", kDefaultHost);
    const int   port       = uvx::config::env_or_cfg_int(cfg, "BENCH_PORT",
                                                          "bench.port", kDefaultPort());
    ctx.conn_count         = uvx::config::env_or_cfg_int(cfg, "BENCH_CONNS",
                                                          "bench.conns", kDefaultConns());
    ctx.duration_sec       = uvx::config::env_or_cfg_int(cfg, "BENCH_DURATION_SEC",
                                                          "bench.duration_sec", kDefaultDurationSec());
    ctx.msg_size           = uvx::config::env_or_cfg_int(cfg, "BENCH_MSG_SIZE",
                                                          "bench.msg_size", kDefaultMsgSize());

    if (ctx.msg_size < 2) {
        ctx.msg_size = 2;
    }

    uvx::log::info("bench: host={}:{} conns={} duration={}s msg_size={}B",
                   host, port, ctx.conn_count, ctx.duration_sec, ctx.msg_size);

    uv_loop_init(&ctx.loop);

    sockaddr_in dest{};
    if (false == resolve_host(&ctx.loop, host, port, &dest)) {
        uv_loop_close(&ctx.loop);
        return 1;
    }

    ctx.conns.reserve(ctx.conn_count);
    ctx.active_conns = ctx.conn_count;

    for (int i = 0; i < ctx.conn_count; ++i) {
        auto* conn    = new BenchConn{};
        conn->conn_id = i;
        conn->ctx     = &ctx;

        uvx::check(uv_tcp_init(&ctx.loop, &conn->socket), "uv_tcp_init");
        conn->socket.data = conn;

        uvx::check(uv_tcp_connect(&conn->connect_req, &conn->socket,
                                  reinterpret_cast<const sockaddr*>(&dest),
                                  on_connect),
                   "uv_tcp_connect");

        ctx.conns.emplace_back(conn);
    }

    uvx::check(uv_timer_init(&ctx.loop, &ctx.deadline), "uv_timer_init");
    ctx.deadline.data = &ctx;
    uvx::check(uv_timer_start(&ctx.deadline, on_deadline,
                              static_cast<uint64_t>(ctx.duration_sec) * 1000, 0),
               "uv_timer_start");

    ctx.start_time_ns = uv_hrtime();

    const int rc = uv_run(&ctx.loop, UV_RUN_DEFAULT);

    print_results(ctx);

    for (auto* c : ctx.conns) delete c;

    uv_loop_close(&ctx.loop);
    return rc;
} catch (const std::exception& e) {
    uvx::log::error("FATAL: {}", e.what());
    return 1;
}
