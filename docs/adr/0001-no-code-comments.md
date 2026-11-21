# 0001. No code comments in src/

Status: accepted, 2026-11-20.

## Context

`src/` held about 38,600 comment-only lines and 9,900 trailing comments (2026-11-20, `python3 tools/comments.py stats`). Many repeated the code, some described code that has since changed, and some let a hack stand with a note instead of a fix. The decisions worth keeping were scattered across headers where nobody looks for them.

## Decision

- Code in `src/` (C, C++, GLSL, Java, Kotlin) carries no comments. Exempt: SPDX license lines, `clang-format on/off`, `NOLINT`, `IWYU pragma`.
- Every comment gets one of four verdicts when it is removed:
  - tautology (repeats the code) or stale (describes code that is gone): delete;
  - excuse (a note that lets a hack stand): delete and file an issue that quotes it;
  - decision record (a non-obvious decision or a fact the code cannot show): move it into an ADR in `docs/adr/`.
- A ratchet enforces the ban: `tools/comment-baseline.txt` lists how many comment lines each file may still have. `python3 tools/comments.py check` fails when a file has more; `update` lowers the baseline and never raises it. New files start at zero. The pre-commit hook checks staged files.
- Removal goes section by section with the `/delete-comment` skill (`.claude/skills/delete-comment/SKILL.md`).

## Consequences

- Decisions live in ADRs named after the code they describe; a reader looks up the symbol there.
- Code ported from upstream (yuzu, Eden, forks) arrives with comments. Strip them in the same change, or the check fails.
- Stripping upstream-derived files makes later cherry-picks from those projects conflict on comment lines. Sections shared with an active upstream go last.
- First section done: the Orchid-own `nx2*` files (409 comment blocks: 269 decision records into ADRs 0002-0007, 117 tautologies, 3 stale, 20 excuses filed as #187-#192).
