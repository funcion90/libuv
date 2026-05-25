#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace uvx::config {

// =========================================================================
//  단순 YAML 파서 — 학습용 (외부 의존성 없음)
//
//  지원:
//    - 한 단계 중첩 (section: \n  key: value)
//    - # 주석
//    - "..." / '...' 따옴표 제거
//    - dotted key 로 평탄화 (server.port 등)
//
//  미지원: 리스트, 다중 중첩, anchors, multi-line 문자열
// =========================================================================

class Config {
public:
    Config() = default;

    bool load(std::string_view path) noexcept {
        std::ifstream f{std::string{path}};
        if (false == f.is_open()) {
            return false;
        }

        loaded_ = true;
        path_   = std::string{path};
        entries_.clear();

        std::string line;
        std::string current_section;
        while (std::getline(f, line)) {
            parse_line(line, current_section);
        }
        return true;
    }

    // 후보 경로 여러 개 시도 — 실행 디렉토리/프로젝트 루트 등
    bool load_first_existing(std::initializer_list<std::string_view> paths) noexcept {
        for (const auto& p : paths) {
            if (load(p)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool               loaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::string& path()   const noexcept { return path_; }

    [[nodiscard]] std::string get_string(const std::string& key,
                                         const std::string& fallback) const noexcept {
        if (auto it = entries_.find(key); it != entries_.end()) {
            return it->second;
        }
        return fallback;
    }

    [[nodiscard]] int get_int(const std::string& key, int fallback) const noexcept {
        if (auto it = entries_.find(key); it != entries_.end()) {
            try {
                return std::stoi(it->second);
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    }

private:
    void parse_line(std::string line, std::string& current_section) noexcept {
        // # 주석 제거
        if (const auto p = line.find('#'); std::string::npos != p) {
            line.erase(p);
        }
        // 끝 \r/\n 제거
        while (false == line.empty() &&
               ('\r' == line.back() || '\n' == line.back())) {
            line.pop_back();
        }

        const auto first = line.find_first_not_of(" \t");
        if (std::string::npos == first) {
            return;  // 공백 라인
        }

        const std::size_t indent  = first;
        const std::string content = line.substr(first);

        const auto colon = content.find(':');
        if (std::string::npos == colon) {
            return;
        }

        std::string key   = content.substr(0, colon);
        std::string value = (colon + 1 < content.size())
                          ? content.substr(colon + 1)
                          : std::string{};

        // value trim
        const auto v_first = value.find_first_not_of(" \t");
        if (std::string::npos == v_first) {
            value.clear();
        } else {
            value = value.substr(v_first);
            const auto v_last = value.find_last_not_of(" \t");
            value = value.substr(0, v_last + 1);
        }

        // 따옴표 제거
        if (value.size() >= 2 &&
            (('"'  == value.front() && '"'  == value.back()) ||
             ('\'' == value.front() && '\'' == value.back()))) {
            value = value.substr(1, value.size() - 2);
        }

        if (value.empty()) {
            if (0 == indent) {
                current_section = key;
            }
        } else {
            const std::string full_key = current_section.empty()
                                       ? key
                                       : current_section + "." + key;
            entries_[full_key] = value;
        }
    }

    bool                                         loaded_ = false;
    std::string                                  path_;
    std::unordered_map<std::string, std::string> entries_;
};

// =========================================================================
//  Lookup 우선순위: 환경변수 > YAML > default
// =========================================================================

inline int env_or_cfg_int(const Config& cfg,
                          const char* env_name,
                          const std::string& cfg_key,
                          int fallback) noexcept {
    if (nullptr != env_name) {
        if (const char* env = std::getenv(env_name); nullptr != env && '\0' != env[0]) {
            try {
                const int n = std::stoi(env);
                return n;
            } catch (...) {
                // env 값이 숫자가 아님 — yaml/fallback 으로
            }
        }
    }
    return cfg.get_int(cfg_key, fallback);
}

inline std::string env_or_cfg_str(const Config& cfg,
                                  const char* env_name,
                                  const std::string& cfg_key,
                                  const std::string& fallback) noexcept {
    if (nullptr != env_name) {
        if (const char* env = std::getenv(env_name); nullptr != env && '\0' != env[0]) {
            return env;
        }
    }
    return cfg.get_string(cfg_key, fallback);
}

}  // namespace uvx::config
