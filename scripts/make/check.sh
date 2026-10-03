#!/usr/bin/env bash
# Before a commit: build everything, run all unit tests, then show what git will see.
#
#   scripts/make/check.sh

source "$(dirname "$0")/common.sh"

cmake_build
say "running unit tests..."
ctest --test-dir "$build_dir" --output-on-failure || fail "unit tests failed"

ok "all checks passed"
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
    say "changes:"
    git status --short
fi
