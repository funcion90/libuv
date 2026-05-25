#pragma once

// =========================================================================
//  uvx::net::MultiReactor
//
//  Master-acceptor + N worker thread-per-loop TCP 서버 framework.
//  - Master(main thread): listen + accept + round-robin fd duplication dispatch
//  - Worker(N threads):   자체 uv_loop, IPC pipe handoff 수신, data echo/handle
//
//  사용자는 ConnectionCallbacks 만 set_callbacks() 로 등록하면 됨.
//  protocol-agnostic 하게 다른 handler 도 같은 framework 위에서 동작.
//
//  사용 예 (echo 서버):
//    uvx::config::Config cfg;
//    cfg.load_first_existing({"uvx.yaml", "config/uvx.yaml"});
//    uvx::net::MultiReactor reactor(cfg);
//    reactor.set_callbacks({
//        .on_data = [](uvx::net::Connection& c, std::string_view data) {
//            c.send(data);
//        }
//    });
//    return reactor.run();
// =========================================================================

#include <uv.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <vector>

#include "uvx/core/config.hpp"
#include "uvx/core/log.hpp"
#include "uvx/core/uv_check.hpp"
#include "uvx/net/connection.hpp"
#include "uvx/net/transferable_socket.hpp"

namespace uvx::net {

#ifdef _WIN32
inline constexpr int          kDumpSignal     = SIGBREAK;
inline constexpr const char*  kDumpSignalName = "SIGBREAK (Ctrl+Break)";
#else
inline constexpr int          kDumpSignal     = SIGUSR1;
inline constexpr const char*  kDumpSignalName = "SIGUSR1";
#endif

struct ConnectionCallbacks {
    // Connection 은 가벼운 wrapper (포인터 1개) 라 by value 로 전달.
    std::function<void(Connection)>                   on_accept;
    std::function<void(Connection, std::string_view)> on_data;
    std::function<void(Connection)>                   on_disconnect;
};

class MultiReactor {
public:
    explicit MultiReactor(const uvx::config::Config& cfg) noexcept {
        const int from_env = uvx::config::env_or_cfg_int(cfg, "ECHO_WORKER_COUNT",
                                                         "server.worker_count", 0);
        if (from_env > 0 && from_env <= 64) {
            worker_count_ = from_env;
        } else {
            const unsigned hw = std::thread::hardware_concurrency();
            worker_count_ = (hw > 1) ? static_cast<int>(hw) - 1 : 1;
        }
        port_     = uvx::config::env_or_cfg_int(cfg, "ECHO_PORT",
                                                 "server.port", 7000);
        backlog_  = uvx::config::env_or_cfg_int(cfg, "ECHO_BACKLOG",
                                                 "server.backlog", 128);
        stats_ms_ = uvx::config::env_or_cfg_int(cfg, "ECHO_STATS_INTERVAL_MS",
                                                 "server.stats_interval_ms", 5000);
    }

    MultiReactor(const MultiReactor&)            = delete;
    MultiReactor& operator=(const MultiReactor&) = delete;

    void set_callbacks(ConnectionCallbacks cbs) noexcept {
        callbacks_ = std::move(cbs);
    }

    int run() noexcept {
        try {
            return run_impl();
        } catch (const std::exception& e) {
            uvx::log::error("MultiReactor FATAL: {}", e.what());
            return 1;
        }
    }

private:
    // -----------------------------------------------------------------
    //  Worker context (per-thread state)
    // -----------------------------------------------------------------
    struct WorkerContext {
        int                             worker_id = 0;
        uv_loop_t                       loop{};
        uv_async_t                      handoff_async{};
        uv_async_t                      shutdown_async{};
        std::mutex                      queue_mutex;
        std::queue<TransferableSocket>  pending;
        std::thread                     thread;
        std::atomic<bool>               running{false};

        std::atomic<int>                live_conns{0};
        std::atomic<uint64_t>           total_conns{0};
        MultiReactor*                   reactor = nullptr;
    };

    // Connection wrapper 는 가벼운 trivial copy 객체 — 매번 stack 에 만든다.
    static Connection wrap(uv_tcp_t* handle) noexcept {
        return Connection{handle};
    }

    // -----------------------------------------------------------------
    //  Worker echo 처리
    // -----------------------------------------------------------------
    static void on_close_client(uv_handle_t* h) noexcept {
        auto* client = reinterpret_cast<uv_tcp_t*>(h);
        if (nullptr != client->data) {
            auto* worker = reinterpret_cast<WorkerContext*>(client->data);
            worker->live_conns.fetch_sub(1, std::memory_order_relaxed);
            if (worker->reactor->callbacks_.on_disconnect) {
                worker->reactor->callbacks_.on_disconnect(wrap(client));
            }
        }
        delete client;
    }

    static void on_alloc(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) noexcept {
        buf->base = new char[suggested];
        buf->len  = static_cast<decltype(buf->len)>(suggested);
    }

    static void on_read_client(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) noexcept {
        auto* tcp    = reinterpret_cast<uv_tcp_t*>(client);
        auto* worker = reinterpret_cast<WorkerContext*>(tcp->data);

        if (nread > 0) {
            if (worker->reactor->callbacks_.on_data) {
                worker->reactor->callbacks_.on_data(
                    wrap(tcp),
                    std::string_view{buf->base, static_cast<std::size_t>(nread)});
            }
            if (nullptr != buf->base) {
                delete[] buf->base;
            }
            return;
        }

        if (nread < 0) {
            if (UV_EOF != nread) {
                uvx::log::warn("read error: {}", uv_strerror(static_cast<int>(nread)));
            }
            if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(client))) {
                uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_client);
            }
        }

        if (nullptr != buf->base) {
            delete[] buf->base;
        }
    }

    static void on_handoff(uv_async_t* async) noexcept {
        auto* worker = reinterpret_cast<WorkerContext*>(async->data);

        std::vector<TransferableSocket> batch;
        {
            std::lock_guard<std::mutex> lk(worker->queue_mutex);
            while (false == worker->pending.empty()) {
                batch.emplace_back(worker->pending.front());
                worker->pending.pop();
            }
        }

        for (const auto& t : batch) {
            auto* client = new uv_tcp_t{};
            if (transferable_to_tcp(t, &worker->loop, client)) {
                client->data = worker;
                worker->live_conns.fetch_add(1, std::memory_order_relaxed);
                worker->total_conns.fetch_add(1, std::memory_order_relaxed);

                if (worker->reactor->callbacks_.on_accept) {
                    worker->reactor->callbacks_.on_accept(wrap(client));
                }

                uv_read_start(reinterpret_cast<uv_stream_t*>(client), on_alloc, on_read_client);
            } else {
                uvx::log::error("worker {} failed to restore socket", worker->worker_id);
                delete client;
            }
        }
    }

    static void on_worker_shutdown(uv_async_t* async) noexcept {
        auto* worker = reinterpret_cast<WorkerContext*>(async->data);
        uvx::log::info("worker {} shutdown signal received", worker->worker_id);

        std::size_t drained = 0;
        {
            std::lock_guard<std::mutex> lk(worker->queue_mutex);
            while (false == worker->pending.empty()) {
                cleanup_transferable(worker->pending.front());
                worker->pending.pop();
                ++drained;
            }
        }
        if (drained > 0) {
            uvx::log::info("worker {} drained {} pending transferable(s)",
                           worker->worker_id, drained);
        }

        uv_walk(&worker->loop,
                +[](uv_handle_t* h, void*) noexcept {
                    if (false == uv_is_closing(h)) {
                        uv_close(h, nullptr);
                    }
                },
                nullptr);
    }

    static void worker_main(WorkerContext* worker) noexcept {
        try {
            uv_loop_init(&worker->loop);

            uvx::check(uv_async_init(&worker->loop, &worker->handoff_async, on_handoff),
                       "worker handoff_async");
            worker->handoff_async.data = worker;

            uvx::check(uv_async_init(&worker->loop, &worker->shutdown_async, on_worker_shutdown),
                       "worker shutdown_async");
            worker->shutdown_async.data = worker;

            worker->running.store(true, std::memory_order_release);
            uvx::log::info("worker {} loop started", worker->worker_id);

            uv_run(&worker->loop, UV_RUN_DEFAULT);
            uv_loop_close(&worker->loop);
            uvx::log::info("worker {} loop stopped", worker->worker_id);
        } catch (const std::exception& e) {
            uvx::log::error("worker {} init FAILED: {}", worker->worker_id, e.what());
            worker->running.store(true, std::memory_order_release);
        }
    }

    // -----------------------------------------------------------------
    //  Master
    // -----------------------------------------------------------------
    static void on_close_dispatched(uv_handle_t* h) noexcept {
        delete reinterpret_cast<uv_tcp_t*>(h);
    }

    void dispatch_client(uv_tcp_t* client) noexcept {
        TransferableSocket t;
        if (false == socket_to_transferable(client, &t)) {
            uvx::log::error("dispatch: socket_to_transferable failed");
            uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_dispatched);
            return;
        }

        const std::size_t idx    = rr_index_++ % workers_.size();
        auto*             worker = workers_[idx];

        {
            std::lock_guard<std::mutex> lk(worker->queue_mutex);
            worker->pending.push(t);
        }
        uv_async_send(&worker->handoff_async);

        uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_dispatched);
    }

    static void on_new_connection(uv_stream_t* server, int status) noexcept {
        if (status < 0) {
            uvx::log::error("listen error: {}", uv_strerror(status));
            return;
        }
        auto* self   = reinterpret_cast<MultiReactor*>(server->data);
        auto* client = new uv_tcp_t{};
        uv_tcp_init(server->loop, client);

        if (0 == uv_accept(server, reinterpret_cast<uv_stream_t*>(client))) {
            self->dispatch_client(client);
        } else {
            uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_dispatched);
        }
    }

    void dump_stats() noexcept {
        int      total_live  = 0;
        uint64_t total_total = 0;
        for (auto* w : workers_) {
            const int      live  = w->live_conns.load(std::memory_order_relaxed);
            const uint64_t total = w->total_conns.load(std::memory_order_relaxed);
            total_live  += live;
            total_total += total;
            uvx::log::info("  worker {:2}  live={:5}  total={}", w->worker_id, live, total);
        }
        uvx::log::info("  TOTAL      live={:5}  total={}", total_live, total_total);
    }

    static void on_stats_timer(uv_timer_t* timer) noexcept {
        auto* self = reinterpret_cast<MultiReactor*>(timer->data);
        uvx::log::info("=== periodic stats ===");
        self->dump_stats();
    }

    static void on_dump_signal(uv_signal_t* sig, int) noexcept {
        auto* self = reinterpret_cast<MultiReactor*>(sig->data);
        uvx::log::info("=== on-demand stats ({}) ===", kDumpSignalName);
        self->dump_stats();
    }

    static void on_sigint(uv_signal_t* sig, int) noexcept {
        auto* self = reinterpret_cast<MultiReactor*>(sig->data);
        uvx::log::info("SIGINT received, broadcasting shutdown to {} worker(s)",
                       self->workers_.size());
        uvx::log::info("=== final stats ===");
        self->dump_stats();

        for (auto* worker : self->workers_) {
            uv_async_send(&worker->shutdown_async);
        }

        uv_walk(&self->master_loop_,
                +[](uv_handle_t* h, void*) noexcept {
                    if (false == uv_is_closing(h)) {
                        uv_close(h, nullptr);
                    }
                },
                nullptr);
    }

    int run_impl() {
        uvx::log::info("starting multi-reactor server with {} worker(s) "
                       "[master-acceptor + fd-duplication]", worker_count_);
        uvx::log::info("port={} backlog={} stats_interval_ms={}", port_, backlog_, stats_ms_);
        uvx::log::info("dump signal: {}", kDumpSignalName);

        uv_loop_init(&master_loop_);

        // worker spawn
        workers_.reserve(worker_count_);
        for (int i = 0; i < worker_count_; ++i) {
            auto* worker      = new WorkerContext{};
            worker->worker_id = i;
            worker->reactor   = this;
            workers_.emplace_back(worker);

            worker->thread = std::thread(worker_main, worker);
            while (false == worker->running.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }

        // listener
        uvx::check(uv_tcp_init(&master_loop_, &server_), "uv_tcp_init");
        server_.data = this;

        sockaddr_in bind_addr{};
        uvx::check(uv_ip4_addr("0.0.0.0", port_, &bind_addr), "uv_ip4_addr");
        uvx::check(uv_tcp_bind(&server_,
                               reinterpret_cast<const sockaddr*>(&bind_addr), 0),
                   "uv_tcp_bind");
        uvx::check(uv_listen(reinterpret_cast<uv_stream_t*>(&server_),
                             backlog_, on_new_connection),
                   "uv_listen");

        // signals + timer
        uvx::check(uv_signal_init(&master_loop_, &sigint_), "uv_signal_init");
        sigint_.data = this;
        uvx::check(uv_signal_start(&sigint_, on_sigint, SIGINT), "uv_signal_start");

        uvx::check(uv_signal_init(&master_loop_, &dump_signal_), "uv_signal_init(dump)");
        dump_signal_.data = this;
        uvx::check(uv_signal_start(&dump_signal_, on_dump_signal, kDumpSignal),
                   "uv_signal_start(dump)");

        if (stats_ms_ > 0) {
            uvx::check(uv_timer_init(&master_loop_, &stats_timer_), "uv_timer_init");
            stats_timer_.data = this;
            uvx::check(uv_timer_start(&stats_timer_, on_stats_timer,
                                      stats_ms_, stats_ms_),
                       "uv_timer_start");
        }

        uvx::log::info("listening on 0.0.0.0:{} (Ctrl+C to stop)", port_);

        const int rc = uv_run(&master_loop_, UV_RUN_DEFAULT);
        uv_loop_close(&master_loop_);

        for (auto* worker : workers_) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
            delete worker;
        }
        workers_.clear();

        uvx::log::info("multi-reactor server stopped");
        return rc;
    }

    // -----------------------------------------------------------------
    //  Master state
    // -----------------------------------------------------------------
    uv_loop_t                   master_loop_{};
    uv_tcp_t                    server_{};
    uv_signal_t                 sigint_{};
    uv_signal_t                 dump_signal_{};
    uv_timer_t                  stats_timer_{};
    std::vector<WorkerContext*> workers_;
    std::size_t                 rr_index_ = 0;

    // Settings
    int worker_count_ = 0;
    int port_         = 7000;
    int backlog_      = 128;
    int stats_ms_     = 5000;

    ConnectionCallbacks callbacks_;
};

}  // namespace uvx::net
