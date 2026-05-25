#pragma once

// =========================================================================
//  uvx::net::Connection
//
//  Worker loop 에 attach 된 client TCP socket 의 사용자 친화 wrapper.
//  사용자 콜백(on_data 등) 안에서 send / close 호출용.
// =========================================================================

#include <uv.h>

#include <cstring>
#include <string_view>

namespace uvx::net {

class Connection {
public:
    explicit Connection(uv_tcp_t* handle) noexcept : handle_(handle) {}

    // Trivial wrapper — copy 가능. handle 수명은 라이브러리가 관리하고
    // Connection 객체 자체는 가벼운 reference 역할.
    Connection(const Connection&)            = default;
    Connection& operator=(const Connection&) = default;
    Connection(Connection&&)                 = default;
    Connection& operator=(Connection&&)      = default;

    uv_tcp_t* raw_handle() const noexcept { return handle_; }

    // 데이터 송신 (copy 후 비동기 write). uv_write 가 자동 cleanup.
    bool send(std::string_view data) noexcept {
        if (data.empty() || nullptr == handle_) {
            return false;
        }

        struct WriteCtx {
            uv_write_t req{};
            uv_buf_t   buf{};
        };

        auto* ctx = new WriteCtx{};
        ctx->buf.base = new char[data.size()];
        std::memcpy(ctx->buf.base, data.data(), data.size());
        ctx->buf.len  = static_cast<decltype(ctx->buf.len)>(data.size());

        const int rc = uv_write(&ctx->req,
                                reinterpret_cast<uv_stream_t*>(handle_),
                                &ctx->buf, 1,
                                +[](uv_write_t* req, int /*status*/) noexcept {
                                    auto* c = reinterpret_cast<WriteCtx*>(req);
                                    delete[] c->buf.base;
                                    delete c;
                                });
        if (rc < 0) {
            delete[] ctx->buf.base;
            delete ctx;
            return false;
        }
        return true;
    }

    void close() noexcept {
        if (nullptr != handle_ &&
            false == uv_is_closing(reinterpret_cast<uv_handle_t*>(handle_))) {
            uv_close(reinterpret_cast<uv_handle_t*>(handle_), nullptr);
        }
    }

private:
    uv_tcp_t* handle_ = nullptr;
};

}  // namespace uvx::net
