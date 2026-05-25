#pragma once

#include <uv.h>

#include <concepts>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace uvx {

template <typename F, typename... Args>
concept UvCallable = std::is_invocable_r_v<int, F, Args...>;

[[nodiscard]] inline std::string err_message(int rc, std::string_view what) {
    return std::format("{} failed: {} ({})", what, uv_strerror(rc), uv_err_name(rc));
}

inline void check(int rc, std::string_view what) {
    if (rc < 0) {
        throw std::runtime_error(err_message(rc, what));
    }
}

[[nodiscard]] inline std::string format_ip4(const sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN]{};
    uv_ip4_name(&addr, buf, sizeof(buf));
    return std::format("{}:{}", buf, ntohs(addr.sin_port));
}

}  // namespace uvx
