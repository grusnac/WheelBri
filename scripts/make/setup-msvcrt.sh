#!/usr/bin/env bash
# Build wheelbri-setup.exe, the installer, as the package ships it: with MSYS2's MINGW64 compiler, against msvcrt.dll
# (in every Windows) instead of the Universal C Runtime, so it starts on a Windows 7, 8 or 8.1 that has no UCRT yet and
# installs it there (installer/setup/SetupSteps.cpp). Into <build dir>-setup/installer/setup/; checks that the exe
# imports no api-ms-win-crt-* DLL. package.sh runs it.
# Needs MSYS2's MINGW64 compiler, once (any MSYS2 shell; this script runs from UCRT64 and finds it in /mingw64/bin):
#   pacman -S mingw-w64-x86_64-gcc
#
#   scripts/make/setup-msvcrt.sh

# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

on_windows || fail "the installer is a Windows program: run it from the MSYS2 UCRT64 shell on Windows"
have_mingw64 || fail "no MINGW64 compiler in /mingw64/bin: pacman -S mingw-w64-x86_64-gcc"

# The MINGW64 compiler's own helpers need its DLLs in /mingw64/bin first on PATH (cmake and ninja still come from
# UCRT64).
export PATH="/mingw64/bin:$PATH"

build_dir="${build_dir}-setup"
WE_CMAKE_ARGS="-DCMAKE_CXX_COMPILER=/mingw64/bin/g++.exe -DCMAKE_RC_COMPILER=/mingw64/bin/windres.exe \
-DWE_BUILD_SERVICE=OFF -DWE_BUILD_SHIM=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF -DWE_BUILD_G923CAP=OFF \
-DWE_BUILD_SETUP=ON ${WE_CMAKE_ARGS:-}"
cmake_build wheelbri-setup
exe="$build_dir/installer/setup/wheelbri-setup.exe"
if objdump -p "$exe" | grep -qi 'DLL Name: api-ms-win-crt-'; then
    fail "$exe imports the Universal C Runtime: it would not start on a Windows 7 without KB2999226"
fi
objdump -p "$exe" | grep -qi 'DLL Name: msvcrt.dll' || fail "$exe does not use msvcrt.dll: built with the wrong compiler?"
ok "installer (msvcrt): $exe"
