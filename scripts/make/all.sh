#!/usr/bin/env bash
# Build everything on Windows: the CMake parts (core, service, the 64- and 32-bit shim, tests) and the drivers.
#
#   scripts/make/all.sh             incremental
#   scripts/make/all.sh --clean     full rebuild of everything (deletes the CMake build tree first)

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
source "$here/common.sh"

clean=0
[ "${1:-}" = "--clean" ] && clean=1

if [ $clean -eq 1 ]; then
    bash "$here/rebuild.sh" || exit 1
else
    bash "$here/app.sh" --no-start || exit 1
fi

if on_windows; then
    if [ $clean -eq 1 ]; then bash "$here/drivers.sh" -Clean; else bash "$here/drivers.sh"; fi || exit 1
fi
ok "everything built"
