#pragma once

#include <chrono>
#include <format>
#include <iostream>
#include <string_view>

namespace uvx::log {

inline void write(std::string_view level, std::string_view msg) {
    const auto now = std::chrono::system_clock::now();
    std::cout << std::format("[{:%H:%M:%S}] [{}] {}\n",
                             std::chrono::floor<std::chrono::seconds>(now),
                             level, msg);
    std::cout.flush();
}

template <typename... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) {
    write("INFO ", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) {
    write("WARN ", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) {
    write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace uvx::log
