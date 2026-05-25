# CLAUDE.md

libuv 학습 샌드박스 프로젝트의 Claude Code 진입점 문서.

---

## 프로젝트 개요

| 항목 | 내용 |
|------|------|
| 종류 | libuv 기반 C++20 비동기 네트워크 학습 샘플 (TCP echo 서버/클라이언트) |
| libuv 버전 | v1.52.1 (CMake `FetchContent`로 자동 다운로드) |
| 빌드 시스템 | CMake 3.20+ |
| 컴파일러 옵션 | MSVC `/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc`, GCC `-Wall -Wextra -Wpedantic` |
| 진입점 | `src/echo_server/main.cpp`, `src/echo_client/main.cpp` |
| 공용 유틸 | `src/common/uv_check.hpp` (`UvCallable` concept), `src/common/log.hpp` |

---

## 작업 전 필수 확인

1. **코드 패턴**: `.claude/rules/cpp-patterns.md` 우선 검토 — 코딩 규약(인코딩/캐스트/Yoda/정렬)이 명문화되어 있음
2. **MD 파일명**: `.claude/rules/md-patterns.md` (kebab-case 강제)
3. **기존 코드 답습**: `src/common/`, `src/echo_server/`, `src/echo_client/`에서 유사 패턴이 있는지 먼저 확인

---

## 필수 규칙

- **언어**: 모든 응답은 한글로 작성
- **C++ 파일**: UTF-8 with BOM, CRLF (`*.cpp`, `*.hpp`, `*.h`, `*.inl` 전체)
- **Bash (Git Bash / MSYS2)**:
  - Windows 옵션의 `/`는 **`//` 로 이스케이프** (예: `cmd //c "..."`, `dir //b`, `findstr //i`)
  - Windows 명령(`dir`, `del`, `type`, `findstr` 등)은 `cmd //c "명령어"` 로 실행
  - 리다이렉션 `> nul`, `2>&1` 사용 금지 (Git Bash에서 `nul` 파일이 실제 생성됨)
  - 가능하면 Unix 명령어(`ls`, `cat`, `grep`) 우선 사용
- **Python**: `python3` 금지 → 반드시 `python` 사용 (Windows PATH에 `python3` 없음)

---

## 프로젝트 구조

```
D:\Projects\libuv\
├── CMakeLists.txt           # 최상위 빌드 정의 (C++20, FetchLibuv 포함)
├── cmake/
│   └── FetchLibuv.cmake     # libuv v1.52.1 FetchContent
├── src/
│   ├── common/              # 공용 헤더 (헤더-온리)
│   │   ├── uv_check.hpp     # UvCallable concept + check() + err_message()
│   │   └── log.hpp
│   ├── echo_server/         # TCP echo 서버 (0.0.0.0:7000)
│   │   └── main.cpp
│   └── echo_client/         # TCP echo 클라이언트
│       └── main.cpp
├── build/                   # CMake 산출물 (커밋 제외)
│   └── _deps/libuv-src/     # FetchContent로 받아진 libuv 원본
└── .claude/
    └── rules/
        ├── cpp-patterns.md  # C++ 코딩 규약
        └── md-patterns.md   # MD 파일명 규약
```

---

## 코드 작성 규칙 인덱스

| 항목 | 위치 |
|------|------|
| C++ 패턴 (인코딩/캐스트/Yoda/정렬/네이밍/주석) | `.claude/rules/cpp-patterns.md` |
| MD 파일명 규약 | `.claude/rules/md-patterns.md` |

---

## 주의사항

- **libuv 본체 수정 금지**: `build/_deps/libuv-src/` 아래는 FetchContent로 받아진 외부 코드. 직접 수정 금지 (다시 받으면 사라짐).
- **libuv C API 경계**: 콜백 시그니처(`uv_alloc_cb`, `uv_read_cb` 등)에 등장하는 `int`, `ssize_t`, `unsigned int`, `uv_buf_t`는 **외부 타입 그대로** 받아쓴다. 내부 도메인 로직에서만 본인이 선호하는 표준 타입(`int32_t`, `uint64_t`)을 골라 일관되게 사용.
- **콜백은 `noexcept`**: libuv 콜백 내부에서 예외가 새어 나가면 미정의 동작. 예외 처리는 콜백 밖 `main()` 경계에서만.

---

## 버전 관리

- **Git** 사용 (libuv 본 프로젝트와 동일)
- `build/`, `build/_deps/` 는 `.gitignore`에 포함
- **커밋 전 사용자 확인 필수** — 자동 커밋 금지

---

## Plan 모드 규칙

계획 파일 작성 시 다음 항목을 반드시 포함:

- 사용자 원본 요청 (수정 없이 그대로 인용)
- 영향받는 범위 (파일/디렉토리)
- 검색 가능한 핵심 키워드 3~5개
- 요구사항을 분해한 상세 목록
- 검증 방법 (실행/관찰로 확인 가능한 단계)
