#!/usr/bin/env bash
# Full rebuild of the CMake parts: deletes the build tree (and build-x86, the 32-bit shim's), configures from scratch
# and builds everything.
# Use it after changing compilers, CMake options or toolchains, or when an incremental build acts up.
# (The drivers too: scripts/make/all.sh --clean.)
#
#   scripts/make/rebuild.sh

# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

# Never the repository, a folder holding it (WE_BUILD_DIR=. or ..), the home folder or a drive's root.
safe_build_dir() {
    local dir repo
    dir="$(cd "$build_dir" 2>/dev/null && pwd -P)" || return 0   # not there yet: nothing to delete
    repo="$(cd "$root" && pwd -P)"
    [ "$dir" != "/" ] && [ "$dir" != "$(cd ~ && pwd -P)" ] && [[ "$repo/" != "$dir"/* ]] && [[ ! "$dir" =~ ^/[a-zA-Z]/?$ ]]
}
[ -n "$build_dir" ] && safe_build_dir || fail "WE_BUILD_DIR ($build_dir) holds the repository or is a system folder: not deleting it"
for dir in "$build_dir" "${build_dir}-x86"; do   # and the 32-bit shim's tree (shim-x86.sh)
    if [ -d "$dir" ]; then
        say "deleting $dir..."
        rm -rf "$dir" || fail "could not delete $dir (is wheelbri.exe or wheelbri-service still running?)"
    fi
done
configure
cmake_build
build_shim_x86
ok "rebuilt: $(service_exe)"
