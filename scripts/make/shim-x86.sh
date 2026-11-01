#!/usr/bin/env bash
# Build the 32-bit dinput8.dll shim, for 32-bit games (Need for Speed Carbon and its era), into build-x86/shim/.
# Only the shim: the app, the service and the drivers stay 64-bit. The app puts this one into a game whose .exe is
# 32-bit (and the 64-bit one into the rest); package.sh ships it as shim\x86\dinput8.dll. app.sh, rebuild.sh, all.sh
# and package.sh run it too: this script alone is for building only the 32-bit shim.
# Needs MSYS2's 32-bit compiler, once (any MSYS2 shell; this script runs from UCRT64 and finds it in /mingw32/bin):
#   pacman -S mingw-w64-i686-gcc
#
#   scripts/make/shim-x86.sh

# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

on_windows || fail "the shim is a Windows DLL: run it from the MSYS2 UCRT64 shell on Windows"
have_mingw32 || fail "no 32-bit compiler in /mingw32/bin: pacman -S mingw-w64-i686-gcc"

# The 32-bit compiler's own helpers (cc1plus, as, ld) need the 32-bit DLLs in /mingw32/bin; with UCRT64's 64-bit ones
# first on PATH they fail without a word ("not able to compile a simple test program"). Configure and build with
# /mingw32/bin first (cmake and ninja still come from UCRT64).
export PATH="/mingw32/bin:$PATH"

build_dir="${build_dir}-x86"
WE_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF \
-DWE_BUILD_LOGI=OFF ${WE_CMAKE_ARGS:-}"
cmake_build wheelbri_shim
ok "32-bit shim: $build_dir/shim/dinput8.dll"
