# Global rules

## Responses
- Minimal verbosity. Prefer a short example over prose.
- After a code change, end with: what changed → build → test (what to check, what failure looks like; say if no rebuild) → git commands last.
- Build/test steps: full command(s) first, then the project's shortcut script if one exists. One command per code block. Name the shell.
- Recurring command sequences → propose a script.
- Feature finished → update user docs and README status in that commit. Ask if unclear.

## Tokens
- Large files: find the section first (`grep -n '^#'` or a keyword grep), then read only that range. Never read a whole file in chunks.
- Never re-read a file or range already in context (CLAUDE.md is auto-loaded; read only the lines you will edit).
- Batch changes to one file into one edit; prefer Edit over rewriting the whole file with Write.
- Narrow listings: specific dirs or `git ls-files <dir>`, not a recursive `find` of the repo.
- No backup copies of tracked files; git is the backup.
- Keep CLAUDE.md files under ~200 lines; move rarely needed detail to `docs/`.

## Environment
- Windows; builds in MSYS2 UCRT64 unless PowerShell is needed.
- Never a bare `script.ps1`: use `powershell -NoProfile -ExecutionPolicy Bypass -File <script.ps1>`.

## Git
- Commit message: short headline + a few lines of why.
- Never add co-authored by claude in the commits.
- Feature branch → commit there, never `main`. Check `.git/HEAD`; start with `git switch <branch>` and `git status`.

## Code
- Comments sparingly, only when necessary: non-obvious intent, constraints, public API. Never restate the code.
- No new warnings. Tests use fakes, independent of order, user and platform.
- Read logs/telemetry before guessing at runtime problems.

## Files and scripts
- LF everywhere; CRLF only for `.cmd`, `.bat`, `.inx`, `.inf`. Never convert a whole file's line endings.
- No Python. Bash (POSIX awk only) or PowerShell / `.cmd` for Windows-only work.
- Scripts never wait at the end (no `pause`, "press Enter", `-NoExit`); ask only for real decisions.
- User docs short; developer details go in code comments and CLAUDE.md.

## Verify, never assume

- Never act on an assumption. Verify every fact before relying on it: code, config, builds, logs, docs, device
  and game behaviour.
- Back every claim with proof: file + line, log line, command output, capture, or source link.
- If something can't be verified yet, say so plainly and say how to verify it; never present it as fact.

## Commands I'm asked to run
- Every command or script must run correctly when pasted as-is into a fresh terminal: no placeholders, no relying on variables or state from an earlier command, no files I must create by hand first.
- Say which shell it is for and whether it needs admin.
- If it needs a tool that may be missing, download or install it inside the same command.
- Target Windows PowerShell 5.1 unless told otherwise.
