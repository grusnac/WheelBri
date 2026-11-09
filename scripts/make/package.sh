#!/usr/bin/env bash
# Build everything the Windows installer ships (app, service, the 64- and 32-bit shim, the installer, the drivers) in
# Release, then assemble dist/windows/WheelBri-<version>/, its zip and WheelBri-<version>-setup.exe (package.ps1).
#
#   scripts/make/package.sh                    build, then package
#   scripts/make/package.sh --no-build         package what is already built
#   scripts/make/package.sh -NoSetupExe        arguments starting with - go to package.ps1

# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

on_windows || fail "the installer package is Windows only (MSYS2 UCRT64 shell)"
[ "$build_type" = "Release" ] || fail "the package is built from Release only (unset WE_BUILD_TYPE)"

build=1
if [ "${1:-}" = "--no-build" ]; then
    build=0
    shift
fi

if [ $build -eq 1 ]; then
    cmake_build wheelbri wheelbri-service wheelbri-helper wheelbri_shim   # wheelbri_shim builds dinput8.dll
    bash "$root/scripts/make/setup-msvcrt.sh" || fail "the installer did not build (setup-msvcrt.sh)"
    have_mingw32 || fail "the package needs the 32-bit shim: pacman -S mingw-w64-i686-gcc"
    build_shim_x86
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$root/scripts/make/drivers.ps1")" \
        || fail "the drivers did not build (Visual Studio + WDK, docs/building.md)"
fi

tree=$build_dir
case "$tree" in /*) ;; *) tree="$root/$tree" ;; esac
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$root/scripts/make/package.ps1")" \
    -BuildDir "$(cygpath -w "$tree")" "$@" || fail "packaging failed"
