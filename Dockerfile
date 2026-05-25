# =========================================================================
#  libuv sandbox — multi-stage build
#  builder: ubuntu + cmake/g++ + libuv FetchContent
#  runtime: ubuntu + libstdc++6 + 3 binaries + uvx.yaml
# =========================================================================

# Stage 1: builder
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 의존성 정의 먼저 복사 (CMakeLists / cmake 폴더만) → libuv FetchContent layer 캐싱
COPY CMakeLists.txt .
COPY cmake/ ./cmake/

# 소스/설정 복사
COPY src/ ./src/
COPY config/ ./config/

# Release 빌드
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --config Release --parallel

# =========================================================================
# Stage 2: runtime (slim)
# =========================================================================
FROM ubuntu:24.04 AS runtime

# bash 는 healthcheck 의 /dev/tcp 사용을 위해 필요 (ubuntu base 에 기본 포함)
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 빌더 stage 에서 바이너리와 yaml 만 복사
COPY --from=builder /src/build/bin/echo_server  ./
COPY --from=builder /src/build/bin/echo_client  ./
COPY --from=builder /src/build/bin/bench_client ./
COPY --from=builder /src/config/uvx.yaml        ./uvx.yaml

EXPOSE 7000

# command 는 docker-compose 에서 service 별로 지정
