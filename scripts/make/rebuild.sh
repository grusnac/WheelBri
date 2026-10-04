#!/usr/bin/env bash
# Full rebuild of the CMake parts: deletes the build tree, configures from scratch and builds everything.
# Use it after changing compilers, CMake options or toolchains, or when an incremental build acts up.
# (The drivers too: scripts/make/all.sh --clean.)
#
#   scripts/make/rebuild.sh

source "$(dirname "$0")/common.sh"

# Never the repository, a folder holding it (WE_BUILD_DIR=. or ..), the home folder or a drive's root.
safe_build_dir() {
    local dir repo
    dir="$(cd "$build_dir" 2>/dev/null && pwd -P)" || return 0   # not there yet: nothing to delete
    repo="$(cd "$root" && pwd -P)"
    [ "$dir" != "/" ] && [ "$dir" != "$(cd ~ && pwd -P)" ] && [[ "$repo/" != "$dir"/* ]] && [[ ! "$dir" =~ ^/[a-zA-Z]/?$ ]]
}
[ -n "$build_dir" ] && safe_build_dir || fail "WE_BUILD_DIR ($build_dir) holds the repository or is a system folder: not deleting it"
if [ -d "$build_dir" ]; then
    say "deleting $build_dir..."
    rm -rf "$build_dir" || fail "could not delete $build_dir (is wheelbri-service still running?)"
fi
configure
cmake_build
ok "rebuilt: $(service_exe)"
