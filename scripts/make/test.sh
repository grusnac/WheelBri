#!/usr/bin/env bash
# Build, then run the unit tests.
#
#   scripts/make/test.sh             all tests
#   scripts/make/test.sh Mapper      only tests whose name matches (ctest -R), e.g. Mapper, EmulatorConfig

source "$(dirname "$0")/common.sh"

cmake_build
args=(--test-dir "$build_dir" --output-on-failure)
[ $# -gt 0 ] && args+=(-R "$1")
say "running tests${1:+ matching '$1'}..."
ctest "${args[@]}" || fail "tests failed"
ok "tests passed"
