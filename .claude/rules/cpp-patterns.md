---
paths:
  - "**/*.{cpp,hpp,h,inl}"
---

# C++ Code Patterns (libuv 학습 프로젝트)

본 문서는 `D:\Projects\libuv` 내 모든 C++ 코드에 적용되는 코딩 규약을 정의한다. libuv C API 경계와 만나는 부분의 외부 타입(`int`, `ssize_t`, `unsigned int`, `uv_buf_t` 등)은 **외부 타입 그대로** 받아쓴다 — 내부 도메인 로직에만 본 규약을 적용한다.

---

## 파일 인코딩 (필수)

**모든 C++ 파일은 UTF-8 with BOM + CRLF**

| 항목 | 규칙 |
|------|------|
| 인코딩 | UTF-8 with BOM (`EF BB BF`) |
| 줄바꿈 | CRLF (`\r\n`) |
| 적용 범위 | `*.cpp`, `*.hpp`, `*.h`, `*.inl` 전체 |

**코드 생성 시**: `open(..., encoding='utf-8-sig', newline='\r\n')` 또는 BOM(`﻿`) 포함하여 파일 출력.

**근거**: MSVC가 한글/일본어 식별자를 안전하게 파싱하도록 BOM 필요. CRLF는 Windows 표준이며 일관성 유지.

---

## 헤더 Include 순서

다음 순서로 그룹화하고, 각 그룹 사이는 빈 줄로 구분한다:

1. **대응 헤더** (`.cpp` 파일이라면 같은 이름의 `.hpp`/`.h`)
2. **libuv 헤더** — `<uv.h>`
3. **C++ 표준 라이브러리** — `<concepts>`, `<format>`, `<string>`, `<cstddef>` 등
4. **프로젝트 공용 헤더** — `"common/uv_check.hpp"`, `"common/log.hpp"`
5. **같은 모듈 내 헤더**

```cpp
// ✅ 올바른 예시 — src/echo_server/main.cpp
#include <uv.h>

#include "common/log.hpp"
#include "common/uv_check.hpp"

#include <cstddef>
#include <cstring>
```

**규칙 근거**: libuv는 OS별 헤더(`<windows.h>` 등)를 끌어오므로 가장 먼저 include 해야 매크로 충돌이 적다.

---

## 변수 명명

| 구분 | 규칙 | 예시 |
|------|------|------|
| private 멤버 | snake_case | `client_socket`, `buffer_size` |
| public 멤버 | PascalCase | `ClientSocket`, `BufferSize` |
| 입력 인자 | `in_` 접두사 | `in_loop`, `in_handle` |
| 출력 인자 | `out_` 접두사 | `out_address`, `out_size` |
| 로컬 변수 | 접두사 없음, snake_case | `peer`, `bind_addr` |
| 상수 (constexpr/consteval) | snake_case 함수 또는 `kPascalCase` | `kPort()`, `kBacklog()` |
| 매크로 | UPPER_SNAKE_CASE | `UNUSED(x)` (가능하면 매크로 회피) |

**근거**: libuv 콜백 시그니처(`uv_handle_t* handle`)와 자연스럽게 어울리도록 인자에 `in_`/`out_` 접두사 강제.

---

## Yoda Condition (필수)

상수/리터럴/`nullptr`/`false`를 **좌변에** 두는 비교문을 사용한다. 실수로 `==` 대신 `=`를 쓴 경우 컴파일러가 즉시 잡아준다.

| 사용 | 사용 금지 |
|------|-----------|
| `if (nullptr == ptr)` | ~~`if (ptr == nullptr)`~~ |
| `if (nullptr != ptr)` | ~~`if (ptr != nullptr)`~~ |
| `if (false == func())` | ~~`if (!func())`~~ |
| `if (true == enabled)` | ~~`if (enabled)`~~ (bool 변수는 그대로 OK, 함수 반환값만 Yoda) |
| `if (0 == rc)` | ~~`if (rc == 0)`~~ (libuv 반환값 체크에 자주 등장) |

```cpp
// ✅ 올바른 예시 — Yoda 비교
if (nullptr == buf->base) {
    return;
}

if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(client))) {
    uv_close(reinterpret_cast<uv_handle_t*>(client), on_close);
}

const int rc = uv_tcp_init(loop, &server);
if (0 != rc) {
    throw std::runtime_error(uvx::err_message(rc, "uv_tcp_init"));
}

// ❌ 잘못된 예시 — Yoda 아님
if (buf->base == nullptr) { /* ... */ }
if (!uv_is_closing(handle)) { /* ... */ }
```

**예외**:
- `if (status < 0)` 같은 **부등호 비교**는 Yoda 적용 안 함 (`if (0 > status)`는 가독성 저하)
- `bool` 변수 그 자체(`if (enabled)`)는 OK — 함수 호출 결과만 Yoda

---

## 타입 캐스팅 (필수)

**C-style 캐스팅 금지** — 반드시 C++ named cast 사용.

| 캐스팅 | 용도 |
|--------|------|
| `static_cast<T>()` | 일반 타입 변환 (정수↔실수, enum↔정수, 축소 변환) |
| `dynamic_cast<T>()` | 다형성 클래스 다운캐스팅 (런타임 검사) |
| `const_cast<T>()` | `const`/`volatile` 제거 |
| `reinterpret_cast<T>()` | 비트 재해석 (포인터↔정수, libuv 핸들 변환) |

**libuv에서 자주 등장하는 캐스트**:

```cpp
// ✅ libuv 핸들 변환 — reinterpret_cast
uv_close(reinterpret_cast<uv_handle_t*>(client), on_close);
uv_read_start(reinterpret_cast<uv_stream_t*>(client), on_alloc, on_read);

// ✅ ssize_t → int 축소 변환 — static_cast
uvx::log::warn("read error: {}", uv_strerror(static_cast<int>(nread)));

// ✅ size_t → unsigned int — static_cast (decltype로 의도 명확화)
buf->len = static_cast<decltype(buf->len)>(suggested);

// ❌ C-style cast 금지
uv_close((uv_handle_t*)client, on_close);
buf->len = (unsigned int)suggested;
```

**`const` 보존 규칙**: `reinterpret_cast` 사용 시 원본이 `const`이면 캐스트 결과도 `const`를 포함시켜야 한다. `reinterpret_cast`로 `const`를 제거하면 컴파일 에러가 발생한다.

```cpp
// ✅ 원본이 const → 결과도 const
const auto* src = peer_data;  // const uint8_t*
stream->append(reinterpret_cast<const char*>(src), len);

// ❌ const 누락 — 컴파일 에러
stream->append(reinterpret_cast<char*>(src), len);
```

---

## 변수 선언 시 `=` 정렬 (필수)

연속된 변수 선언에서 `=` 기호를 세로로 정렬한다.

```cpp
// ✅ 올바른 예시 — = 정렬
const auto* loop        = uv_default_loop();
const auto  port        = kPort();
const auto  backlog     = kBacklog();
const auto  client_addr = format_ip4(peer);

// ❌ 잘못된 예시 — 정렬 안됨
const auto* loop = uv_default_loop();
const auto port = kPort();
const auto backlog = kBacklog();
```

**적용 범위**: 인접한 2개 이상의 선언이 같은 의미적 블록에 속할 때. 무관한 선언을 억지로 정렬하지 않는다.

---

## 수치 한계값 (필수)

**C 매크로 대신 `std::numeric_limits<T>` 사용**.

| 사용 | 사용 금지 |
|------|-----------|
| `std::numeric_limits<int32_t>::max()` | ~~`INT32_MAX`~~ |
| `std::numeric_limits<int32_t>::min()` | ~~`INT32_MIN`~~ |
| `std::numeric_limits<uint64_t>::max()` | ~~`UINT64_MAX`~~ / ~~`ULLONG_MAX`~~ |
| `std::numeric_limits<size_t>::max()` | ~~`SIZE_MAX`~~ |
| `std::numeric_limits<float>::max()` | ~~`FLT_MAX`~~ |

```cpp
// ✅ 올바른 예시
#include <limits>

constexpr auto max_message_size = std::numeric_limits<size_t>::max();
const auto    sentinel          = std::numeric_limits<int32_t>::min();

// ❌ 잘못된 예시 — C 매크로 사용 금지
constexpr auto max_message_size = SIZE_MAX;
const auto    sentinel          = INT32_MIN;
```

**근거**: 템플릿 친화적, 타입 변경 시 자동 추적, 헤더 의존성 명확(`<limits>` 단일 출처).

---

## 비트플래그 비교

`enum class` (scoped enum)의 `operator&` / `operator|`는 기본 비활성이다. 비트 연산이 필요하면:

**옵션 1 — 명시적 캐스트** (가장 안전):

```cpp
// ✅ underlying_type으로 명시 캐스트
enum class EventMask : uint32_t {
    Read  = 1 << 0,
    Write = 1 << 1,
    Error = 1 << 2,
};

using U = std::underlying_type_t<EventMask>;
if ((static_cast<U>(mask) & static_cast<U>(EventMask::Read)) != 0) {
    // ...
}
```

**옵션 2 — 헬퍼 함수** (반복 사용 시):

```cpp
// 헤더 1곳에 정의
template <typename E>
[[nodiscard]] constexpr bool HasFlag(E lhs, E rhs) noexcept
    requires std::is_enum_v<E>
{
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(lhs) & static_cast<U>(rhs)) == static_cast<U>(rhs);
}

// 사용
if (HasFlag(mask, EventMask::Read)) { /* ... */ }
```

**금지**: `if (mask & EventMask::Read)` — scoped enum에서 컴파일 에러.

---

## 전방선언 / Shared Pointer 별칭

`std::shared_ptr<T>` / `std::weak_ptr<T>`를 자주 사용하는 타입에는 **외부 별칭**을 정의한다.

**별칭 규칙**:
- `ClassPtr`  = `std::shared_ptr<Class>`
- `ClassWPtr` = `std::weak_ptr<Class>`

**클래스 내부 정의 금지**:

```cpp
// ❌ 클래스 내부에서 SharedPtr 정의 금지
class TcpSession {
public:
    using SharedPtr = std::shared_ptr<TcpSession>;  // 제거
};

// ✅ 클래스 외부에서 using으로 정의
class TcpSession { /* ... */ };
using TcpSessionPtr  = std::shared_ptr<TcpSession>;
using TcpSessionWPtr = std::weak_ptr<TcpSession>;
```

**정의 위치**:
1. 같은 헤더 내 클래스 정의 직후
2. 또는 공용 헤더(`common/forwards.hpp` 등 — 필요 시 신설)

**순환 참조 방지**: 소유권이 없는 쪽은 `weak_ptr` 사용.

---

## 함수 주석 (XML 문서 주석)

**주석 위치 원칙**: `.cpp`에 상세 주석, `.hpp`에는 간단한 한 줄 주석 또는 생략.

### 헤더 (`.hpp`) — 간결하게

함수명과 인자명만으로 의미가 충분하면 주석 생략. 필요 시 `//` 한 줄 주석만 작성.

```cpp
// libuv 에러 코드를 사람이 읽기 좋은 문자열로 변환
[[nodiscard]] std::string err_message(int rc, std::string_view what);

// 음수 반환값을 std::runtime_error로 변환
void check(int rc, std::string_view what);
```

### 구현 (`.cpp`) — 상세하게

복잡한 분기/주의사항은 `.cpp`에 XML 문서 주석으로 기록.

```cpp
/// <summary>
/// libuv 에러 코드를 사람이 읽기 좋은 문자열로 포맷팅한다.
/// </summary>
/// <param name="rc">libuv 반환 코드 (음수면 에러)</param>
/// <param name="what">실패한 작업 이름 (예: "uv_tcp_init")</param>
/// <returns>"what failed: 에러설명 (UV_ECODE)" 형식 문자열</returns>
std::string err_message(int rc, std::string_view what) {
    return std::format("{} failed: {} ({})", what, uv_strerror(rc), uv_err_name(rc));
}
```

**태그 규칙** (`.cpp`):

| 태그 | 용도 | 필수 여부 |
|------|------|-----------|
| `<summary>` | 함수 기능 요약 | 필수 |
| `<param>` | 매개변수 설명 | 매개변수 있을 때 필수 |
| `<returns>` | 반환값 설명 | `void` 아닐 때 필수 |
| `<remarks>` | 추가 설명/주의사항 | 선택 |

---

## libuv 콜백 패턴

libuv의 콜백은 C 함수 포인터로 등록되므로 몇 가지 강제 규칙이 있다.

### 1. `noexcept` 필수

libuv 콜백 안에서 예외가 새어 나가면 **미정의 동작**. 모든 콜백은 `noexcept`로 선언한다.

```cpp
// ✅ 올바른 예시
void on_close(uv_handle_t* handle) noexcept {
    delete reinterpret_cast<uv_tcp_t*>(handle);
}

void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) noexcept {
    // ...
}

// ❌ noexcept 누락 — std::format 등에서 예외 전파 가능
void on_close(uv_handle_t* handle) {
    auto msg = std::format("...");  // bad_alloc 가능
    delete reinterpret_cast<uv_tcp_t*>(handle);
}
```

### 2. 익명 namespace로 internal linkage

같은 콜백 이름이 다른 TU에서 충돌하지 않도록 익명 namespace 또는 `static`으로 묶는다.

```cpp
// ✅ 올바른 예시 — 익명 namespace
namespace {

void on_close(uv_handle_t* handle) noexcept { /* ... */ }
void on_write(uv_write_t* req, int status) noexcept { /* ... */ }

}  // namespace
```

### 3. 예외 처리 경계

예외는 `main()` 또는 그에 준하는 진입점에서만 catch.

```cpp
int main() {
    try {
        // libuv 초기화 + uv_run
    } catch (const std::exception& e) {
        uvx::log::error("fatal: {}", e.what());
        return 1;
    }
    return 0;
}
```

---

## libuv 리소스 수명 관리

libuv는 비동기 종료를 요구하는 핸들이 많아 `delete`를 `uv_close` 콜백에서 처리하는 패턴이 표준이다.

### `new` ↔ `uv_close → on_close → delete` 페어 유지

```cpp
// ✅ 올바른 패턴
auto* client = new uv_tcp_t{};
uv_tcp_init(server->loop, client);

// 종료 시
uv_close(reinterpret_cast<uv_handle_t*>(client), on_close);

void on_close(uv_handle_t* handle) noexcept {
    delete reinterpret_cast<uv_tcp_t*>(handle);
}
```

### 이미 closing 중인지 확인

`uv_close`를 두 번 호출하면 안 된다.

```cpp
if (false == uv_is_closing(reinterpret_cast<uv_handle_t*>(client))) {
    uv_close(reinterpret_cast<uv_handle_t*>(client), on_close);
}
```

### Write 요청 구조체

`uv_write_t` 요청도 콜백에서 `delete` 한다.

```cpp
struct WriteReq {
    uv_write_t req{};
    uv_buf_t   buf{};
};

void on_write(uv_write_t* req, int status) noexcept {
    auto* w = reinterpret_cast<WriteReq*>(req);
    if (0 > status) {
        uvx::log::error("write error: {}", uv_strerror(status));
    }
    delete[] w->buf.base;
    delete w;
}
```

### RAII로 감쌀 수 있다면 우선 사용

`uv_loop_t`처럼 동기적으로 정리 가능한 객체는 RAII 래퍼를 만들거나 스택에 두는 것이 안전하다. 비동기 종료가 필요한 핸들(`uv_tcp_t`, `uv_timer_t` 등)은 위 페어 패턴을 따른다.

---

## 시간 측정 (필요 시)

libuv는 고해상도 시간 측정 API를 제공한다.

```cpp
const uint64_t start_ns = uv_hrtime();
// ... 작업 ...
const uint64_t elapsed_ns = uv_hrtime() - start_ns;
uvx::log::info("elapsed: {} ms", elapsed_ns / 1'000'000);
```

**주의**: `uv_hrtime()`은 단조 증가 시간이므로 절대 시각/날짜 표시에는 사용 불가. 절대 시각이 필요하면 `<chrono>`의 `system_clock`을 사용.
