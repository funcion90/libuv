#pragma once

// =========================================================================
//  uvx::bench::Runner
//
//  TCP echo-style server 에 대한 RTT/throughput 벤치마크 framework.
//  - N 커넥션 동시 connect
//  - 각 conn 이 deadline 까지 echo round-trip 반복
//  - 종료 후 RTT min/avg/max + req/s + MB/s 집계
//
//  사용 예:
//    uvx::bench::RunnerConfig cfg;
//    cfg.host         = "127.0.0.1";
//    cfg.port         = 7000;
//    cfg.conn_count   = 32;
//    cfg.duration_sec = 10;
//    cfg.msg_size     = 64;
//
//    uvx::bench::Runner runner(cfg);
//    const auto result = runner.run();
//    result.print();   // 또는 result.rps / result.rtt_avg_us 직접 접근
// =========================================================================

#include <uv.h>

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

namespace uvx::bench {

// =========================================================================
//  Config + Result struct
// =========================================================================

struct RunnerConfig {
    std::string host         = "127.0.0.1";
    int         port         = 7000;
    int         conn_count   = 16;
    int         duration_sec = 10;
    int         msg_size     = 64;
};

// yaml/env 우선순위 적용 (env > yaml > default)
inline RunnerConfig load_config(const uvx::config::Config& cfg) noexcept {
    RunnerConfig rc;
    rc.host         = uvx::config::env_or_cfg_str(cfg, "BENCH_HOST",          "bench.host",         rc.host);
    rc.port         = uvx::config::env_or_cfg_int(cfg, "BENCH_PORT",          "bench.port",         rc.port);
    rc.conn_count   = uvx::config::env_or_cfg_int(cfg, "BENCH_CONNS",         "bench.conns",        rc.conn_count);
    rc.duration_sec = uvx::config::env_or_cfg_int(cfg, "BENCH_DURATION_SEC",  "bench.duration_sec", rc.duration_sec);
    rc.msg_size     = uvx::config::env_or_cfg_int(cfg, "BENCH_MSG_SIZE",      "bench.msg_size",     rc.msg_size);
    if (rc.msg_size < 2) {
        rc.msg_size = 2;
    }
    return rc;
}

struct Result {
    double   elapsed_sec = 0.0;
    int      conn_count  = 0;
    int      msg_size    = 0;
    uint64_t total_req   = 0;
    uint64_t total_sent  = 0;
    uint64_t total_recv  = 0;
    double   rps         = 0.0;
    double   mb_per_sec  = 0.0;
    double   rtt_avg_us  = 0.0;
    double   rtt_min_us  = 0.0;
    double   rtt_max_us  = 0.0;

    void print() const noexcept {
        uvx::log::info("===== Benchmark Result =====");
        uvx::log::info("  duration:     {:.2f} sec", elapsed_sec);
        uvx::log::info("  connections:  {}", conn_count);
        uvx::log::info("  message size: {} bytes", msg_size);
        uvx::log::info("  total req:    {}", total_req);
        uvx::log::info("  throughput:   {:.0f} req/s   {:.2f} MB/s (tx+rx)", rps, mb_per_sec);
        uvx::log::info("  RTT avg:      {:.1f} us  min: {:.1f} us  max: {:.1f} us",
                       rtt_avg_us, rtt_min_us, rtt_max_us);
        uvx::log::info("============================");
    }
};

// =========================================================================
//  Runner
// =========================================================================

class Runner {
public:
    explicit Runner(RunnerConfig cfg) noexcept : cfg_(std::move(cfg)) {}

    Runner(const Runner&)            = delete;
    Runner& operator=(const Runner&) = delete;

    Result run() noexcept {
        try {
            return run_impl();
        } catch (const std::exception& e) {
            uvx::log::error("Runner FATAL: {}", e.what());
            return {};
        }
    }

private:
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
        Runner*           runner    = nullptr;
    };

    // -----------------------------------------------------------------
    //  Helper: host 를 numeric IP 또는 DNS lookup 으로 sockaddr 변환
    // -----------------------------------------------------------------
    static bool resolve_host(uv_loop_t* loop, const std::string& host, int port,
                             sockaddr_in* out) noexcept {
        if (port < 1 || port > 65535) {
            uvx::log::error("invalid port {} (must be 1-65535)", port);
            return false;
        }
        if (0 == uv_ip4_addr(host.c_str(), port, out)) {
            return true;
        }

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
        out->sin_port = htons(static_cast<uint16_t>(port));
        uv_freeaddrinfo(req.addrinfo);
        return true;
    }

    // -----------------------------------------------------------------
    //  Callbacks
    // -----------------------------------------------------------------
    static void on_alloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) noexcept {
        buf->base = new char[suggested];
        buf->len  = static_cast<decltype(buf->len)>(suggested);
    }

    static void on_close_conn(uv_handle_t* handle) noexcept {
        auto* sock = reinterpret_cast<uv_tcp_t*>(handle);
        auto* conn = reinterpret_cast<BenchConn*>(sock->data);
        auto* self = conn->runner;

        self->active_conns_--;
        if (self->active_conns_ <= 0) {
            if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(&self->deadline_))) {
                uv_close(reinterpret_cast<uv_handle_t*>(&self->deadline_), nullptr);
            }
        }
    }

    static void close_conn(BenchConn* conn) noexcept {
        if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(&conn->socket))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&conn->socket), on_close_conn);
        }
    }

    static void on_write(uv_write_t*, int status) noexcept {
        if (status < 0) {
            uvx::log::warn("write error: {}", uv_strerror(status));
        }
    }

    static void send_next(BenchConn* conn) noexcept {
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

    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) noexcept {
        auto* sock = reinterpret_cast<uv_tcp_t*>(stream);
        auto* conn = reinterpret_cast<BenchConn*>(sock->data);
        auto* self = conn->runner;

        if (nread < 0) {
            if (UV_EOF != nread && false == self->shutting_down_) {
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

                if (self->shutting_down_) {
                    close_conn(conn);
                    break;
                }
                send_next(conn);
                break;
            }
        }

        if (nullptr != buf->base) delete[] buf->base;
    }

    static void on_connect(uv_connect_t* req, int status) noexcept {
        auto* sock = reinterpret_cast<uv_tcp_t*>(req->handle);
        auto* conn = reinterpret_cast<BenchConn*>(sock->data);

        if (status < 0) {
            uvx::log::error("conn {} connect failed: {}", conn->conn_id, uv_strerror(status));
            close_conn(conn);
            return;
        }

        conn->connected = true;
        conn->send_data.assign(static_cast<std::size_t>(conn->runner->cfg_.msg_size), 'A');
        conn->send_data.back() = '\n';

        uvx::check(uv_read_start(reinterpret_cast<uv_stream_t*>(sock), on_alloc, on_read),
                   "uv_read_start");
        send_next(conn);
    }

    static void on_deadline(uv_timer_t* timer) noexcept {
        auto* self = reinterpret_cast<Runner*>(timer->data);
        uvx::log::info("deadline reached, draining {} connection(s)...",
                       self->active_conns_);
        self->shutting_down_ = true;

        for (auto* c : self->conns_) {
            close_conn(c);
        }
    }

    Result run_impl() {
        uv_loop_init(&loop_);

        sockaddr_in dest{};
        if (false == resolve_host(&loop_, cfg_.host, cfg_.port, &dest)) {
            uv_loop_close(&loop_);
            return {};
        }

        conns_.reserve(cfg_.conn_count);
        active_conns_ = cfg_.conn_count;

        for (int i = 0; i < cfg_.conn_count; ++i) {
            auto* conn    = new BenchConn{};
            conn->conn_id = i;
            conn->runner  = this;

            uvx::check(uv_tcp_init(&loop_, &conn->socket), "uv_tcp_init");
            conn->socket.data = conn;

            uvx::check(uv_tcp_connect(&conn->connect_req, &conn->socket,
                                      reinterpret_cast<const sockaddr*>(&dest),
                                      on_connect),
                       "uv_tcp_connect");

            conns_.emplace_back(conn);
        }

        uvx::check(uv_timer_init(&loop_, &deadline_), "uv_timer_init");
        deadline_.data = this;
        uvx::check(uv_timer_start(&deadline_, on_deadline,
                                  static_cast<uint64_t>(cfg_.duration_sec) * 1000, 0),
                   "uv_timer_start");

        start_time_ns_ = uv_hrtime();

        uv_run(&loop_, UV_RUN_DEFAULT);

        // Aggregate
        Result result;
        result.conn_count = cfg_.conn_count;
        result.msg_size   = cfg_.msg_size;
        const uint64_t end_ns = uv_hrtime();
        result.elapsed_sec    = static_cast<double>(end_ns - start_time_ns_) / 1e9;

        uint64_t rtt_min = std::numeric_limits<uint64_t>::max();
        uint64_t rtt_max = 0;
        uint64_t rtt_sum = 0;

        for (const auto* c : conns_) {
            result.total_req  += c->req_count;
            result.total_sent += c->bytes_sent;
            result.total_recv += c->bytes_recv;
            rtt_sum           += c->rtt_sum_ns;
            if (c->rtt_min_ns < rtt_min) rtt_min = c->rtt_min_ns;
            if (c->rtt_max_ns > rtt_max) rtt_max = c->rtt_max_ns;
        }

        result.rps        = (result.elapsed_sec > 0)
            ? static_cast<double>(result.total_req) / result.elapsed_sec
            : 0.0;
        result.mb_per_sec = (result.elapsed_sec > 0)
            ? static_cast<double>(result.total_sent + result.total_recv)
                / result.elapsed_sec / (1024.0 * 1024.0)
            : 0.0;
        result.rtt_avg_us = (result.total_req > 0)
            ? static_cast<double>(rtt_sum) / result.total_req / 1000.0
            : 0.0;
        result.rtt_min_us = (rtt_min != std::numeric_limits<uint64_t>::max())
            ? static_cast<double>(rtt_min) / 1000.0 : 0.0;
        result.rtt_max_us = static_cast<double>(rtt_max) / 1000.0;

        for (auto* c : conns_) delete c;
        conns_.clear();
        uv_loop_close(&loop_);

        return result;
    }

    // -----------------------------------------------------------------
    //  State
    // -----------------------------------------------------------------
    RunnerConfig             cfg_;
    uv_loop_t                loop_{};
    uv_timer_t               deadline_{};
    std::vector<BenchConn*>  conns_;
    uint64_t                 start_time_ns_ = 0;
    bool                     shutting_down_ = false;
    int                      active_conns_  = 0;
};

}  // namespace uvx::bench
