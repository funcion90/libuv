#include <uv.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <unistd.h>
#endif

#include "common/config.hpp"
#include "common/log.hpp"
#include "common/uv_check.hpp"

namespace {

// =========================================================================
//  Default 값 (yaml/env 미설정 시 사용)
// =========================================================================

consteval int kDefaultPort()            { return 7000; }
consteval int kDefaultBacklog()         { return 128; }
consteval int kDefaultStatsIntervalMs() { return 5000; }

#ifdef _WIN32
constexpr int         kDumpSignal     = SIGBREAK;
constexpr const char* kDumpSignalName = "SIGBREAK (Ctrl+Break)";
#else
constexpr int         kDumpSignal     = SIGUSR1;
constexpr const char* kDumpSignalName = "SIGUSR1";
#endif

int decide_worker_count(const uvx::config::Config& cfg) noexcept {
    const int n = uvx::config::env_or_cfg_int(cfg, "ECHO_WORKER_COUNT", "server.worker_count", 0);
    if (n > 0 && n <= 64) {
        return n;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return (hw > 1) ? static_cast<int>(hw) - 1 : 1;
}

// =========================================================================
//  Transferable Socket (fd duplication)
// =========================================================================

struct TransferableSocket {
#ifdef _WIN32
    WSAPROTOCOL_INFOW info{};
#else
    int fd = -1;
#endif
};

bool socket_to_transferable(uv_tcp_t* src, TransferableSocket* out) noexcept {
    uv_os_fd_t handle{};
    if (0 != uv_fileno(reinterpret_cast<uv_handle_t*>(src), &handle)) {
        return false;
    }

#ifdef _WIN32
    const SOCKET sock = reinterpret_cast<SOCKET>(handle);
    return 0 == WSADuplicateSocketW(sock, GetCurrentProcessId(), &out->info);
#else
    out->fd = ::dup(handle);
    return out->fd >= 0;
#endif
}

#ifdef _WIN32
// WSADuplicateSocket 으로 올린 underlying refcount 를 감소시키는 cleanup.
// WSASocket 으로 변환 후 즉시 closesocket → refcount-- → underlying 해제.
void cleanup_transferable(const TransferableSocket& t) noexcept {
    const SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                const_cast<WSAPROTOCOL_INFOW*>(&t.info),
                                0, 0);
    if (INVALID_SOCKET != s) {
        closesocket(s);
    }
}
#else
void cleanup_transferable(const TransferableSocket& t) noexcept {
    if (t.fd >= 0) {
        ::close(t.fd);
    }
}
#endif

bool transferable_to_tcp(const TransferableSocket& t, uv_loop_t* loop, uv_tcp_t* out) noexcept {
#ifdef _WIN32
    const SOCKET sock = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                   const_cast<WSAPROTOCOL_INFOW*>(&t.info),
                                   0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == sock) {
        // WSASocket 실패 — WSADuplicateSocket 으로 올린 refcount 를 다른 방식으로 cleanup
        cleanup_transferable(t);
        return false;
    }
    if (0 != uv_tcp_init(loop, out)) {
        closesocket(sock);
        return false;
    }
    if (0 != uv_tcp_open(out, sock)) {
        closesocket(sock);
        return false;
    }
    return true;
#else
    if (0 != uv_tcp_init(loop, out)) {
        ::close(t.fd);
        return false;
    }
    if (0 != uv_tcp_open(out, t.fd)) {
        ::close(t.fd);
        return false;
    }
    return true;
#endif
}

// =========================================================================
//  공용 구조체
// =========================================================================

struct WriteReq {
    uv_write_t req{};
    uv_buf_t   buf{};
};

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
};

struct MasterContext {
    uv_loop_t                   loop{};
    uv_tcp_t                    server{};
    uv_signal_t                 sigint{};
    uv_signal_t                 dump_signal{};
    uv_timer_t                  stats_timer{};
    std::vector<WorkerContext*> workers;
    std::size_t                 rr_index = 0;
};

// =========================================================================
//  Worker 측 echo 처리
// =========================================================================

void on_close_client(uv_handle_t* handle) noexcept {
    auto* client = reinterpret_cast<uv_tcp_t*>(handle);
    if (nullptr != client->data) {
        auto* worker = reinterpret_cast<WorkerContext*>(client->data);
        worker->live_conns.fetch_sub(1, std::memory_order_relaxed);
    }
    delete client;
}

void on_write_echo(uv_write_t* req, int status) noexcept {
    auto* w = reinterpret_cast<WriteReq*>(req);
    if (status < 0) {
        uvx::log::error("write error: {}", uv_strerror(status));
    }
    delete[] w->buf.base;
    delete w;
}

void on_alloc(uv_handle_t* /*h*/, std::size_t suggested, uv_buf_t* buf) noexcept {
    buf->base = new char[suggested];
    buf->len  = static_cast<decltype(buf->len)>(suggested);
}

void on_read_client(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) noexcept {
    if (nread > 0) {
        auto*     w  = new WriteReq{};
        w->buf       = uv_buf_init(buf->base, static_cast<unsigned int>(nread));
        const int rc = uv_write(&w->req, client, &w->buf, 1, on_write_echo);
        if (rc < 0) {
            uvx::log::error("uv_write submit failed: {}", uv_strerror(rc));
            delete[] w->buf.base;
            delete w;
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

    if (nullptr != buf->base && nread <= 0) {
        delete[] buf->base;
    }
}

void on_handoff(uv_async_t* async) noexcept {
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
            uv_read_start(reinterpret_cast<uv_stream_t*>(client),
                          on_alloc, on_read_client);
        } else {
            uvx::log::error("worker {} failed to restore socket", worker->worker_id);
            delete client;
        }
    }
}

void on_worker_shutdown(uv_async_t* async) noexcept {
    auto* worker = reinterpret_cast<WorkerContext*>(async->data);
    uvx::log::info("worker {} shutdown signal received", worker->worker_id);

    // pending queue drain — master 가 dispatch 했지만 아직 처리 못 한 transferable 을 close.
    // 이게 없으면 WSADuplicateSocket/dup 으로 올린 refcount 가 누수.
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

void worker_main(WorkerContext* worker) noexcept {
    try {
        uv_loop_init(&worker->loop);

        uvx::check(uv_async_init(&worker->loop, &worker->handoff_async, on_handoff),
                   "worker handoff_async init");
        worker->handoff_async.data = worker;

        uvx::check(uv_async_init(&worker->loop, &worker->shutdown_async, on_worker_shutdown),
                   "worker shutdown_async init");
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

// =========================================================================
//  Master 측
// =========================================================================

void on_close_dispatched(uv_handle_t* h) noexcept {
    delete reinterpret_cast<uv_tcp_t*>(h);
}

void dispatch_client(MasterContext* master, uv_tcp_t* client) noexcept {
    TransferableSocket t;
    if (false == socket_to_transferable(client, &t)) {
        uvx::log::error("dispatch: socket_to_transferable failed");
        uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_dispatched);
        return;
    }

    const std::size_t idx    = master->rr_index++ % master->workers.size();
    auto*             worker = master->workers[idx];

    {
        std::lock_guard<std::mutex> lk(worker->queue_mutex);
        worker->pending.push(t);
    }
    uv_async_send(&worker->handoff_async);

    uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_dispatched);
}

void on_new_connection(uv_stream_t* server, int status) noexcept {
    if (status < 0) {
        uvx::log::error("listen error: {}", uv_strerror(status));
        return;
    }

    auto* master = reinterpret_cast<MasterContext*>(server->data);
    auto* client = new uv_tcp_t{};
    uv_tcp_init(server->loop, client);

    if (0 == uv_accept(server, reinterpret_cast<uv_stream_t*>(client))) {
        dispatch_client(master, client);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(client), on_close_dispatched);
    }
}

void dump_stats(MasterContext* master) noexcept {
    int      total_live  = 0;
    uint64_t total_total = 0;
    for (auto* w : master->workers) {
        const int      live  = w->live_conns.load(std::memory_order_relaxed);
        const uint64_t total = w->total_conns.load(std::memory_order_relaxed);
        total_live  += live;
        total_total += total;
        uvx::log::info("  worker {:2}  live={:5}  total={}", w->worker_id, live, total);
    }
    uvx::log::info("  TOTAL      live={:5}  total={}", total_live, total_total);
}

void on_stats_timer(uv_timer_t* timer) noexcept {
    auto* master = reinterpret_cast<MasterContext*>(timer->data);
    uvx::log::info("=== periodic stats ===");
    dump_stats(master);
}

void on_dump_signal(uv_signal_t* sig, int /*signum*/) noexcept {
    auto* master = reinterpret_cast<MasterContext*>(sig->data);
    uvx::log::info("=== on-demand stats ({}) ===", kDumpSignalName);
    dump_stats(master);
}

void on_sigint(uv_signal_t* sig, int /*signum*/) noexcept {
    auto* master = reinterpret_cast<MasterContext*>(sig->data);
    uvx::log::info("SIGINT received, broadcasting shutdown to {} worker(s)",
                   master->workers.size());

    uvx::log::info("=== final stats ===");
    dump_stats(master);

    for (auto* worker : master->workers) {
        uv_async_send(&worker->shutdown_async);
    }

    uv_walk(&master->loop,
            +[](uv_handle_t* h, void*) noexcept {
                if (false == uv_is_closing(h)) {
                    uv_close(h, nullptr);
                }
            },
            nullptr);
}

}  // namespace

int main() try {
    // ---- Config 로드 ----
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
    } else {
        uvx::log::info("config not found, using env vars + code defaults");
    }

    // ---- 설정 값 결정 (env > yaml > default) ----
    const int worker_count = decide_worker_count(cfg);
    const int port         = uvx::config::env_or_cfg_int(cfg, "ECHO_PORT",
                                                          "server.port", kDefaultPort());
    const int backlog      = uvx::config::env_or_cfg_int(cfg, "ECHO_BACKLOG",
                                                          "server.backlog", kDefaultBacklog());
    const int stats_ms     = uvx::config::env_or_cfg_int(cfg, "ECHO_STATS_INTERVAL_MS",
                                                          "server.stats_interval_ms",
                                                          kDefaultStatsIntervalMs());

    MasterContext master;

    uvx::log::info("starting echo server with {} worker reactor(s) "
                   "[master-acceptor + fd-duplication]", worker_count);
    uvx::log::info("port={} backlog={} stats_interval_ms={}", port, backlog, stats_ms);
    uvx::log::info("dump signal: {}", kDumpSignalName);

    uv_loop_init(&master.loop);

    // ---- Worker spawn ----
    master.workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        auto* worker      = new WorkerContext{};
        worker->worker_id = i;
        master.workers.emplace_back(worker);

        worker->thread = std::thread(worker_main, worker);
        while (false == worker->running.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    // ---- Listener ----
    uvx::check(uv_tcp_init(&master.loop, &master.server), "uv_tcp_init");
    master.server.data = &master;

    sockaddr_in bind_addr{};
    uvx::check(uv_ip4_addr("0.0.0.0", port, &bind_addr), "uv_ip4_addr");
    uvx::check(uv_tcp_bind(&master.server,
                           reinterpret_cast<const sockaddr*>(&bind_addr), 0),
               "uv_tcp_bind");
    uvx::check(uv_listen(reinterpret_cast<uv_stream_t*>(&master.server),
                         backlog, on_new_connection),
               "uv_listen");

    // ---- Signal/Timer ----
    uvx::check(uv_signal_init(&master.loop, &master.sigint), "uv_signal_init");
    master.sigint.data = &master;
    uvx::check(uv_signal_start(&master.sigint, on_sigint, SIGINT), "uv_signal_start");

    uvx::check(uv_signal_init(&master.loop, &master.dump_signal), "uv_signal_init(dump)");
    master.dump_signal.data = &master;
    uvx::check(uv_signal_start(&master.dump_signal, on_dump_signal, kDumpSignal),
               "uv_signal_start(dump)");

    if (stats_ms > 0) {
        uvx::check(uv_timer_init(&master.loop, &master.stats_timer), "uv_timer_init");
        master.stats_timer.data = &master;
        uvx::check(uv_timer_start(&master.stats_timer, on_stats_timer,
                                  stats_ms, stats_ms),
                   "uv_timer_start");
    }

    uvx::log::info("listening on 0.0.0.0:{} (Ctrl+C to stop)", port);

    const int rc = uv_run(&master.loop, UV_RUN_DEFAULT);
    uv_loop_close(&master.loop);

    for (auto* worker : master.workers) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
        delete worker;
    }

    uvx::log::info("server stopped");
    return rc;
} catch (const std::exception& e) {
    uvx::log::error("FATAL: {}", e.what());
    return 1;
}
