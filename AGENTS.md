# AGENTS.md

Short rules for local agents. Full detail is in `CLAUDE.md`: list its headings first
(`grep -n '^#' CLAUDE.md`) and read only the section you need.

## Shell

- Build, test, git and file work in MSYS2 UCRT64 from the repo root:
  `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c "<command>"`
- PowerShell only when there is no other way.

## Build, then test (two steps)

- First time only: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build 2>&1 | grep -E 'error|FAILED' | head -20` (no output = built)
- Test: `ctest --test-dir build 2>&1 | grep -E 'tests passed|FAILED|Failed' | head -20`
- Never tail or print full build or test logs.

## Ask first

- `driver/`, `installer/`, anything needing admin rights.
- FFB safety (`FfbSafetyLimiter`, watchdog): never weaken it.
- No third-party libraries or tools (no SDL, HidHide, ViGEm, HIDMaestro).

## Code

C++20, 4-space indent, `camelCase` functions/variables, `PascalCase` types, `trailing_` members, `kConstant`
constants, `#pragma once`. Logging only through `WE_LOG_*`. No allocations or blocking calls in `Mapper::map`,
`Pipeline::tick`, `encode`.

## Git

- Current branch only. Never commit, push, reset, stash or branch yourself.
- When done: show `git status -s` and `git diff --stat`, then one `git add <files> && git commit -m "<short message>"`
  per small change. No attribution lines.
- Never delete files: list them for the user.
