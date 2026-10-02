#!/usr/bin/env bash
# Full rebuild of the CMake parts: deletes the build tree, configures from scratch and builds everything.
# Use it after changing compilers, CMake options or toolchains, or when an incremental build acts up.
# (The bridge and the drivers: scripts/make/all.sh --clean.)
#
#   scripts/make/rebuild.sh

source "$(dirname "$0")/common.sh"

[ -n "$build_dir" ] && [ "$build_dir" != "/" ] || fail "WE_BUILD_DIR is not a safe folder to delete"
if [ -d "$build_dir" ]; then
    say "deleting $build_dir..."
    rm -rf "$build_dir" || fail "could not delete $build_dir (is wheelemu-service still running?)"
fi
configure
cmake_build
ok "rebuilt: $(service_exe)"
