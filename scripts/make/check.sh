#!/usr/bin/env bash
# Before a commit: build everything, run all unit tests and the bridge's wire self-test, then show what
# git will see.
#
#   scripts/make/check.sh

source "$(dirname "$0")/common.sh"

cmake_build
say "running unit tests..."
ctest --test-dir "$build_dir" --output-on-failure || fail "unit tests failed"

if dotnet=$(dotnet_exe); then
    say "running the bridge wire self-test (tools/vbridge/SelfTest)..."
    "$dotnet" run --project tools/vbridge/SelfTest -c Release -v q -nologo || fail "SelfTest failed"
else
    warn "dotnet not found: skipped tools/vbridge/SelfTest"
fi

ok "all checks passed"
if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
    say "changes:"
    git status --short
fi
