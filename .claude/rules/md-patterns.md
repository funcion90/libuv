---
paths:
  - ".claude/**/*.md"
  - "**/*.md"
---

# MD Patterns

## 파일명 규칙 (Naming Convention)

모든 `.claude/` 내 마크다운 파일은 **kebab-case** 사용:

| 규칙 | 예시 |
|------|------|
| 소문자 + 하이픈(`-`) 구분자 | `cpp-patterns.md`, `md-patterns.md` ✅ |
| 한글 단어도 허용 (하이픈으로 구분) | `libuv-콜백-패턴.md`, `tcp-에코-서버.md` ✅ |
| 언더스코어(`_`) 사용 금지 | `cpp_patterns.md` ❌ |
| camelCase / PascalCase 금지 | `CppPatterns.md` ❌ |
| 확장자: 반드시 `.md` 소문자 | `readme.md` ✅ / `readme.MD` ❌ |

**예외 (대문자 고정명 허용)**:

| 파일명 | 위치 | 이유 |
|--------|------|------|
| `CLAUDE.md` | 프로젝트 루트 | Claude Code 규약 |
| `README.md` | 모든 위치 | 오픈소스/GitHub 관례 |
| `MEMORY.md` | `memory/` | Claude Code 메모리 인덱스 규약 |
| `SKILL.md` | `.claude/skills/*/` | Claude Code 스킬 진입점 고정명 |

---

## 권장 구조

`.claude/rules/` 아래 마크다운은 선택적으로 다음 front matter를 가질 수 있다:

```markdown
---
paths:
  - "**/*.{cpp,hpp,h,inl}"
---
```

`paths:`는 이 규칙이 어떤 파일 종류에 적용되는지 명시한다 (가독성/툴링용).
