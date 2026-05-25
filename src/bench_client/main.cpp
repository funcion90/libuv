// =========================================================================
//  bench_client — uvx::bench::Runner 사용 예제
//
//  라이브러리(uvx/bench/runner.hpp) 가 N 커넥션 동시 connect + RTT 측정 +
//  통계 집계 모두 처리. 사용자는 RunnerConfig 만 채우면 됨.
// =========================================================================

#include "uvx/bench/runner.hpp"
#include "uvx/core/config.hpp"
#include "uvx/core/log.hpp"

int main() {
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

    const auto bench_cfg = uvx::bench::load_config(cfg);
    uvx::log::info("bench: host={}:{} conns={} duration={}s msg_size={}B",
                   bench_cfg.host, bench_cfg.port, bench_cfg.conn_count,
                   bench_cfg.duration_sec, bench_cfg.msg_size);

    uvx::bench::Runner runner(bench_cfg);
    const auto         result = runner.run();
    result.print();

    return 0;
}
