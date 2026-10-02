#!/usr/bin/env bash
# Build everything on Windows: the CMake parts (core, service, shim, tests), the C# bridge and probes, and the
# drivers. Steps whose tools are missing (dotnet, Visual Studio + WDK) are skipped with a warning.
#
#   scripts/make/all.sh             incremental
#   scripts/make/all.sh --clean     full rebuild of everything (deletes the CMake build tree first)

here=$(cd "$(dirname "$0")" && pwd)
source "$here/common.sh"

clean=0
[ "${1:-}" = "--clean" ] && clean=1

if [ $clean -eq 1 ]; then
    bash "$here/rebuild.sh" || exit 1
else
    bash "$here/build.sh" || exit 1
fi

if dotnet_exe >/dev/null; then
    if [ $clean -eq 1 ]; then bash "$here/bridge.sh" --clean; else bash "$here/bridge.sh"; fi || exit 1
else
    warn "dotnet not found: skipped the bridge and probes"
fi

if on_windows; then
    if [ $clean -eq 1 ]; then bash "$here/drivers.sh" -Clean; else bash "$here/drivers.sh"; fi || exit 1
fi
ok "everything built"
