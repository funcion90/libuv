#pragma once

// =========================================================================
//  uvx::net::TransferableSocket
//
//  Master thread 에서 accept 된 socket 을 worker thread 의 별도 uv_loop 로
//  안전하게 이전(ownership transfer)하기 위한 cross-platform fd duplication.
//
//  - Windows: WSADuplicateSocket → WSAPROTOCOL_INFOW (refcount++)
//  - POSIX:   dup(fd) (새 file descriptor)
// =========================================================================

#include <uv.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace uvx::net {

struct TransferableSocket {
#ifdef _WIN32
    WSAPROTOCOL_INFOW info{};
#else
    int fd = -1;
#endif
};

// uv_tcp_t (master 측 handle) 의 underlying socket 을 transferable 형태로 추출.
// master 측 handle 은 이후 close 해도 worker 가 받기 전까지 underlying 유지.
inline bool socket_to_transferable(uv_tcp_t* src, TransferableSocket* out) noexcept {
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

// transferable 을 worker loop 에 attach 된 새 uv_tcp_t 로 변환.
inline bool transferable_to_tcp(const TransferableSocket& t,
                                uv_loop_t*                loop,
                                uv_tcp_t*                 out) noexcept;

// transferable 을 변환하지 않고 close (worker shutdown drain 시 사용).
// Windows: WSADuplicateSocket 이 올린 refcount 를 감소 / POSIX: dup 한 fd close.
inline void cleanup_transferable(const TransferableSocket& t) noexcept {
#ifdef _WIN32
    const SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                const_cast<WSAPROTOCOL_INFOW*>(&t.info),
                                0, 0);
    if (INVALID_SOCKET != s) {
        closesocket(s);
    }
#else
    if (t.fd >= 0) {
        ::close(t.fd);
    }
#endif
}

inline bool transferable_to_tcp(const TransferableSocket& t,
                                uv_loop_t*                loop,
                                uv_tcp_t*                 out) noexcept {
#ifdef _WIN32
    const SOCKET sock = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                   const_cast<WSAPROTOCOL_INFOW*>(&t.info),
                                   0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCKET == sock) {
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

}  // namespace uvx::net
