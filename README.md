# libuv C++20 Network Sandbox

[libuv](https://github.com/libuv/libuv) **v1.52.1** 기반의 비동기 네트워크 학습/실습 sandbox.
C++20 표준 + CMake `FetchContent`로 의존성을 자동으로 가져옵니다.

> ⚠️ 이 저장소는 **libuv 본가가 아니라 libuv를 사용한 학습 sandbox** 입니다.
> libuv 자체 소스를 찾으시는 분은 [libuv/libuv](https://github.com/libuv/libuv) 로 가세요.

## 주요 기능

- **Multi-reactor TCP echo 서버** — master-acceptor + N worker, fd duplication (Linux `dup()` / Windows `WSADuplicateSocket`)
- **자체 벤치마크 클라이언트** — 동시 N 커넥션, RTT/throughput 측정
- **YAML config 시스템** — 자체 단순 파서, 환경변수 override 지원
- **Atomic stats + signal dump** — Linux `SIGUSR1` / Windows `SIGBREAK` + 주기 timer
- **Docker / docker-compose 지원** — 게임서버 / 테스트서버 분리 실행
- **Cross-platform** — Windows (MSVC 2022) + Linux (gcc/clang)

## 측정 결과 (Linux Release, 5 worker)

```
duration:     30.00 sec
connections:  32   (Docker network 격리)
throughput:   118,905 req/s   14.51 MB/s
RTT avg:      269.1 us  min: 17.8 us  max: 35.7 ms
worker 분산:  8/8/7/7/7  (round-robin, healthcheck 포함)
```

> 측정 환경: Windows 11 + Docker Desktop (WSL2 backend), Release 빌드, loopback 네트워크. 호스트 CPU/메모리에 따라 결과는 달라집니다.

## 요구 사항

| 도구 | 버전 |
|------|------|
| CMake | ≥ 3.20 |
| Git | 최신 (FetchContent 사용) |
| C++20 컴파일러 | MSVC 2022 (v143) / GCC 13+ / Clang 16+ |
| Docker (선택) | Engine 24+ / Compose v2 |

## 디렉터리 구조

```
libuv/
├── CMakeLists.txt
├── cmake/
│   └── FetchLibuv.cmake               # libuv v1.52.1 다운로드
├── config/
│   └── uvx.yaml                       # 모든 설정 한 곳
├── src/
│   ├── uvx/                           # ★ 재사용 가능 라이브러리 (헤더-온리)
│   │   ├── core/
│   │   │   ├── config.hpp             #   YAML 파서 + env_or_cfg_* 헬퍼
│   │   │   ├── log.hpp                #   std::format 로깅
│   │   │   └── uv_check.hpp           #   에러 체크 + UvCallable concept
│   │   └── net/
│   │       ├── transferable_socket.hpp #  fd duplication (Win/Linux 분기)
│   │       ├── connection.hpp         #   Connection wrapper (send/close)
│   │       └── multi_reactor.hpp      #   Master + N Worker framework
│   ├── echo_server/main.cpp           # uvx 사용 예제 (35줄)
│   ├── echo_client/main.cpp           # stdin / multi-conn 모드
│   └── bench_client/main.cpp          # RTT/throughput 벤치
├── Dockerfile                         # Multi-stage build (~80MB image)
├── docker-compose.yml                 # echo_server + bench_client (profile)
├── .claude/rules/                     # C++ / MD 코딩 규약 (학습 자료)
└── build/                             # CMake 산출물 (gitignored)
```

## 빠른 시작 (Docker)

가장 빠른 방법:

```bash
git clone https://github.com/funcion90/libuv.git
cd libuv

# 빌드 (~3-5분 첫 실행)
docker compose build

# 게임서버 띄우기
docker compose up echo_server -d
docker compose logs -f echo_server

# 다른 터미널에서 벤치마크
docker compose --profile bench up bench_client --abort-on-container-exit

# 정리
docker compose down
```

## 로컬 빌드

### Windows (Visual Studio 2022)

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

산출물: `build/bin/Debug/echo_server.exe`, `echo_client.exe`, `bench_client.exe`

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

산출물: `build/bin/echo_server`, `echo_client`, `bench_client`

첫 빌드 시 `build/_deps/libuv-src/` 에 libuv v1.52.1이 shallow clone으로 다운로드됩니다.

## 사용법 (바이너리 실행)

### Echo 서버 + 단일 클라이언트 (stdin 모드)

```bash
# 터미널 1
./echo_server

# 터미널 2
./echo_client 127.0.0.1 7000
# 문자열 입력 후 Enter → 서버가 echo 회신
# 종료: Ctrl+Z + Enter (Windows EOF) / Ctrl+D (Linux)
```

### Echo 서버 + 멀티 conn 클라이언트 (분산 확인용)

```bash
# 서버
./echo_server

# 클라이언트 (N개 동시 ping-pong)
ECHO_CLIENT_CONNS=16 ./echo_client
```

### 벤치마크

```bash
# 서버
./echo_server

# 벤치 (yaml의 bench.* 적용)
./bench_client

# 또는 환경변수 override
BENCH_CONNS=64 BENCH_DURATION_SEC=30 ./bench_client
```

## 라이브러리로 사용 (uvx)

이 sandbox 의 `src/uvx/` 는 **헤더-온리 라이브러리**로, 다른 프로젝트에서 그대로 가져다 쓸 수 있습니다.
echo_server / echo_client / bench_client 는 모두 이 라이브러리의 사용 예제입니다.

### 다른 프로젝트에 통합

```bash
# Option 1: git submodule
git submodule add https://github.com/funcion90/libuv.git libs/libuv-sandbox

# Option 2: 수동 복사
cp -r /path/to/libuv-sandbox/src/uvx my-project/libs/uvx
```

```cmake
# 내 프로젝트의 CMakeLists.txt
add_subdirectory(libs/libuv-sandbox)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE uvx_common)
```

`uvx_common` INTERFACE 타겟은 libuv 본체(`uv_a`) + `src/uvx/` include 경로를 함께 노출합니다.

### 최소 예제 — 35줄 echo 서버

```cpp
#include "uvx/core/config.hpp"
#include "uvx/core/log.hpp"
#include "uvx/net/multi_reactor.hpp"

int main() {
    uvx::config::Config cfg;
    cfg.load_first_existing({"uvx.yaml", "config/uvx.yaml"});

    uvx::net::MultiReactor reactor(cfg);

    uvx::net::ConnectionCallbacks cbs;
    cbs.on_data = [](uvx::net::Connection c, std::string_view data) {
        c.send(data);  // echo back
    };
    reactor.set_callbacks(std::move(cbs));

    return reactor.run();   // SIGINT 받을 때까지 blocking
}
```

다른 프로토콜(HTTP/WebSocket 등) 구현 시 `on_data` 람다 안에서 파싱/핸들링만 바꾸면 됩니다.
master-acceptor + N worker + fd duplication + atomic stats + signal dump 는 라이브러리가 모두 처리.

### ConnectionCallbacks API

| 콜백 | 시그니처 | 호출 시점 |
|------|---------|----------|
| `on_accept` | `void(Connection)` | accept 직후 (per-connection 초기화용) |
| `on_data` | `void(Connection, std::string_view)` | 데이터 수신 |
| `on_disconnect` | `void(Connection)` | 연결 종료 직전 (cleanup용) |

모두 optional `std::function`. 필요한 것만 설정. 미설정 시 무시.

### Connection 클래스

| 메서드 | 동작 |
|--------|------|
| `bool send(std::string_view data)` | 비동기 전송 (라이브러리가 메모리 자동 cleanup) |
| `void close()` | 연결 종료 (on_disconnect 콜백 트리거) |
| `uv_tcp_t* raw_handle()` | 저수준 접근 (per-conn state 직접 관리 등) |

`Connection` 은 가벼운 wrapper (포인터 1개) — by value 전달 비용 무시 가능.

### 벤치마크 라이브러리 (uvx::bench::Runner)

서버 부하 측정도 같은 라이브러리에 포함됩니다. `uvx/bench/runner.hpp` 를 include 하면 어떤 TCP echo-style 서버에도 RTT/throughput 측정 가능:

```cpp
#include "uvx/bench/runner.hpp"
#include "uvx/core/config.hpp"

int main() {
    uvx::config::Config cfg;
    cfg.load_first_existing({"uvx.yaml"});

    const auto bench_cfg = uvx::bench::load_config(cfg);  // yaml/env 자동 적용
    uvx::bench::Runner runner(bench_cfg);
    const auto result = runner.run();
    result.print();                                        // 또는 직접 접근

    // result.rps, result.rtt_avg_us, result.total_req 등 사용자 활용
    return 0;
}
```

`RunnerConfig` (host/port/conn_count/duration_sec/msg_size) 만 채우면 N 커넥션 동시 connect + 메시지 ping-pong + 통계 집계가 자동.

### YAML/환경변수 설정 (라이브러리가 자동 적용)

`MultiReactor(cfg)` 생성 시 다음 항목 자동 적용:

| YAML key | 환경변수 | 기본값 |
|----------|---------|--------|
| `server.worker_count` | `ECHO_WORKER_COUNT` | `hardware_concurrency - 1` |
| `server.port` | `ECHO_PORT` | `7000` |
| `server.backlog` | `ECHO_BACKLOG` | `128` |
| `server.stats_interval_ms` | `ECHO_STATS_INTERVAL_MS` | `5000` |

## 설정 (config/uvx.yaml)

```yaml
server:
  port: 7000
  backlog: 128
  worker_count: 5
  stats_interval_ms: 5000

client:
  host: 127.0.0.1
  port: 7000
  conns: 0   # 0=stdin, N>0=multi-mode

bench:
  host: 127.0.0.1
  port: 7000
  conns: 16
  duration_sec: 10
  msg_size: 64
```

**우선순위**: 환경변수 > YAML > 코드 default. 예를 들어 `ECHO_WORKER_COUNT=8`을 export하면 yaml의 `5`를 무시하고 8 적용.

## 통계 Dump

서버 실행 중 다음 방법으로 worker별 connection 카운터 확인:

```bash
# Linux
kill -USR1 $(pidof echo_server)

# Windows (Ctrl+Break in console)
# 또는 5초마다 자동 dump (yaml의 server.stats_interval_ms)
```

출력 예시:
```
=== periodic stats ===
  worker  0  live=    7  total=42
  worker  1  live=    6  total=41
  worker  2  live=    7  total=42
  worker  3  live=    6  total=41
  worker  4  live=    6  total=40
  TOTAL      live=   32  total=206
```

## 아키텍처

```
                  ┌─────────────┐
                  │   Master    │  signal + stats + listener
                  │ uv_loop_t   │
                  └──────┬──────┘
                         │ accept
                  ┌──────▼──────────┐
                  │ fd duplication   │  WSADuplicateSocket / dup
                  │ round-robin push │
                  └──────┬──────────┘
        ┌────────────────┼────────────────┐
   uv_async       uv_async         uv_async
   + mutex+queue  + mutex+queue    + mutex+queue
        │                │                │
   ┌────▼─────┐    ┌────▼─────┐    ┌────▼─────┐
   │ Worker 0 │    │ Worker 1 │    │ Worker N │
   │ uv_loop  │    │ uv_loop  │    │ uv_loop  │
   │ on_read/ │    │ on_read/ │    │ on_read/ │
   │ on_write │    │ on_write │    │ on_write │
   └──────────┘    └──────────┘    └──────────┘
```

## 핵심 학습 포인트

- **libuv 콜백 = C 함수 포인터**: 캡처 없는 람다 또는 자유 함수만. 상태는 `uv_handle_t::data` 또는 익명 namespace.
- **핸들 수명**: `uv_close(handle, on_close)` 의 close 콜백 안에서 `delete`. 즉시 `delete` 하면 후속 콜백이 dangling 참조.
- **Multi-reactor 한계**: master accept + fd duplication이 bottleneck. 진정한 균등 분산은 Linux `SO_REUSEPORT`로 가능 (코드에 `UV_TCP_REUSEPORT` 플래그 분기 존재).
- **`UvCallable` concept**: `uv_check.hpp`에 정의. libuv API 시그니처를 compile-time 검증.
- **fd duplication**: Windows `WSADuplicateSocket` + `WSASocket(FROM_PROTOCOL_INFO)`, Linux `dup()`. 같은 프로세스 thread 간 socket ownership 이전 표준 패턴.

## 코딩 규약

`.claude/rules/` 아래에 C++ / MD 코딩 규약이 명문화되어 있습니다:
- UTF-8 with BOM + CRLF
- Yoda condition (`nullptr == ptr`, `false == func()`)
- Named cast 강제 (`static_cast`, `reinterpret_cast` 등)
- 변수 선언 시 `=` 세로 정렬
- 콜백 `noexcept` 의무

자세한 내용은 [.claude/rules/cpp-patterns.md](.claude/rules/cpp-patterns.md) 참고.

## 라이선스

[MIT License](LICENSE) — libuv 본가와 동일.
