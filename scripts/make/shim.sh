#!/usr/bin/env bash
# Build the dinput8.dll shim and put it next to a game's .exe, or take it out again.
# The shim and the virtual G920 (--virtual-g920) must not be used together.
#
#   scripts/make/shim.sh "D:\Games\Need for Speed"          build and copy dinput8.dll (and wheelbri-shim.ini
#                                                           if the game folder has none)
#   scripts/make/shim.sh --off "D:\Games\Need for Speed"    rename the game's dinput8.dll to dinput8.dll.off

source "$(dirname "$0")/common.sh"

off=0
if [ "${1:-}" = "--off" ]; then
    off=1
    shift
fi
[ $# -eq 1 ] || fail "usage: scripts/make/shim.sh [--off] <game folder>"
game=$1
on_windows && game=$(cygpath -u "$game")
[ -d "$game" ] || fail "no folder at $1"

if [ $off -eq 1 ]; then
    if [ -f "$game/dinput8.dll" ]; then
        mv -f "$game/dinput8.dll" "$game/dinput8.dll.off" || fail "could not rename (is the game running?)"
        ok "shim off: $game/dinput8.dll.off"
    else
        warn "no dinput8.dll in $game: nothing to do"
    fi
    exit 0
fi

on_windows || fail "the shim is a Windows DLL: build it on Windows, or cross-compile (README, Build)"
cmake_build wheelbri_shim   # builds dinput8.dll
cp -f "$build_dir/shim/dinput8.dll" "$game/" || fail "could not copy (is the game running?)"
ok "copied dinput8.dll to $game"
if [ ! -f "$game/wheelbri-shim.ini" ]; then
    cp config/wheelbri-shim.ini "$game/" && ok "copied wheelbri-shim.ini (defaults)"
fi
