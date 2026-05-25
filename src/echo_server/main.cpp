// =========================================================================
//  echo_server — uvx::net::MultiReactor 사용 예제
//
//  라이브러리(uvx/net/multi_reactor.hpp) 가 master-acceptor + N worker +
//  fd duplication + atomic stats + signal dump 모두 처리.
//  사용자는 ConnectionCallbacks::on_data 만 채우면 됨.
// =========================================================================

#include "uvx/core/config.hpp"
#include "uvx/core/log.hpp"
#include "uvx/net/multi_reactor.hpp"

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
    } else {
        uvx::log::info("config not found, using env vars + code defaults");
    }

    uvx::net::MultiReactor reactor(cfg);

    uvx::net::ConnectionCallbacks cbs;
    cbs.on_data = [](uvx::net::Connection c, std::string_view data) {
        c.send(data);
    };
    reactor.set_callbacks(std::move(cbs));

    return reactor.run();
}
