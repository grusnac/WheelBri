#!/usr/bin/env bash
# Build the dinput8.dll shim and put it into a game's folder the way the app's Games tab does, or take it out again.
# The shim and the virtual wheel (--virtual-wheel) must not be used together.
#
# - A 32-bit game (the folder's largest .exe is a 32-bit Windows program) gets the 32-bit shim,
#   <build dir>-x86/shim/dinput8.dll (built here by shim-x86.sh; needs MSYS2's 32-bit compiler,
#   pacman -S mingw-w64-i686-gcc); any other game the 64-bit one, <build dir>/shim/dinput8.dll.
# - Next to the Ultimate ASI Loader (the folder's dinput8.dll is the loader) the shim goes in as dinput8Hooked.dll,
#   which the loader loads in turn. The loader itself is never touched.
# - Another mod's dinput8.dll (not identical to WheelBri's, and no wheelbri-shim.ini next to it) is left alone:
#   nothing is put in.
# - config/wheelbri-shim.ini is copied in only when the folder has none (your own settings are kept).
#
#   scripts/make/shim.sh "D:\Games\Need for Speed"          build the shim and put it in
#   scripts/make/shim.sh --off "D:\Games\Need for Speed"    rename WheelBri's shim there to <name>.off
#                                                           (dinput8.dll.off, or dinput8Hooked.dll.off next to the
#                                                           ASI Loader); wheelbri-shim.ini stays

here=$PWD   # common.sh goes to the repository: a relative game folder is relative to where this was started
# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

off=0
if [ "${1:-}" = "--off" ]; then
    off=1
    shift
fi
[ $# -eq 1 ] || fail "usage: scripts/make/shim.sh [--off] <game folder>"
game=$1
on_windows && game=$(cygpath -u "$game")
[[ "$game" = /* ]] || game="$here/$game"
[ -d "$game" ] || fail "no folder at $1"

# The same decisions as the app (core/src/ShimGames.cpp) and scripts/linux/wheelbri.sh (pe_machine, game_bits,
# shim_name_in).

# A Windows program's processor, from its PE header: 014c (32-bit), 8664 (64-bit), nothing for anything else.
pe_machine() {
    local file=$1 offset
    [ "$(head -c 2 "$file" 2>/dev/null)" = MZ ] || return 0
    offset=$(od -An -tu4 -j60 -N4 "$file" 2>/dev/null | tr -d ' \n')
    [ -n "$offset" ] || return 0
    [ "$(od -An -tx1 -j"$offset" -N4 "$file" 2>/dev/null | tr -d ' \n')" = 50450000 ] || return 0   # "PE\0\0"
    od -An -tx2 -j$((offset + 4)) -N2 "$file" 2>/dev/null | tr -d ' \n'
}

# 32 or 64: the game's .exe in the folder (the largest one, as the app decides); 64 when none says.
game_bits() {
    local exe size largest=-1 bits=64 machine
    for exe in "$1"/*.[eE][xX][eE]; do
        [ -f "$exe" ] || continue
        size=$(stat -c %s "$exe" 2>/dev/null) || continue
        [ "$size" -ge "$largest" ] || continue
        machine=$(pe_machine "$exe") || machine=
        case "$machine" in
            014c) bits=32; largest=$size ;;
            8664) bits=64; largest=$size ;;
        esac
    done
    echo "$bits"
}

# Whether the folder's dinput8.dll is the Ultimate ASI Loader (its export's name is in the file). WheelBri's shim has
# that name too (it looks for the loader): one with the shim's mark (IsWheelBriShim) is ours.
asi_loader_in() {
    [ -f "$1/dinput8.dll" ] && grep -qa IsUltimateASILoader "$1/dinput8.dll" 2>/dev/null &&
        ! grep -qa IsWheelBriShim "$1/dinput8.dll" 2>/dev/null
}

# The name the shim takes in the folder: dinput8.dll, or dinput8Hooked.dll next to the Ultimate ASI Loader.
shim_name_in() {
    if asi_loader_in "$1"; then echo dinput8Hooked.dll; else echo dinput8.dll; fi
}

has_exe() {
    local exe
    for exe in "$1"/*.[eE][xX][eE]; do [ -f "$exe" ] && return 0; done
    return 1
}

shim_x64="$build_dir/shim/dinput8.dll"
shim_x86="${build_dir}-x86/shim/dinput8.dll"
name=$(shim_name_in "$game")
dll="$game/$name"
ini="$game/wheelbri-shim.ini"

if [ $off -eq 1 ]; then
    if [ ! -f "$dll" ]; then
        if asi_loader_in "$game"; then
            warn "no $name in $game (its dinput8.dll is the Ultimate ASI Loader: left alone): nothing to do"
        else
            warn "no dinput8.dll in $game: nothing to do"
        fi
        exit 0
    fi
    # Ours: identical to a built shim (64- or 32-bit), or next to a wheelbri-shim.ini (an older WheelBri shim).
    if ! { [ -f "$shim_x64" ] && cmp -s "$shim_x64" "$dll"; } &&
       ! { [ -f "$shim_x86" ] && cmp -s "$shim_x86" "$dll"; } && [ ! -f "$ini" ]; then
        fail "$dll is not WheelBri's (another mod?): left alone"
    fi
    # Renamed, never deleted; an earlier .off (perhaps not ours) is kept too.
    target="$dll.off"
    n=2
    while [ -e "$target" ]; do target="$dll.off-$n"; n=$((n + 1)); done
    mv "$dll" "$target" || fail "could not rename $dll (is the game running?)"
    ok "shim off: $target"
    exit 0
fi

on_windows || fail "the shim is a Windows DLL: build it on Windows, or cross-compile (README, Building)"
has_exe "$game" || fail "no .exe in $game: pick the folder that holds the game's .exe"
bits=$(game_bits "$game")
if [ "$bits" = 32 ]; then
    have_mingw32 || fail "the game in $game is 32-bit and needs the 32-bit shim, but MSYS2's 32-bit compiler is" \
        "missing: pacman -S mingw-w64-i686-gcc"
    bash "$root/scripts/make/shim-x86.sh" || fail "the 32-bit shim did not build"
    shim=$shim_x86
else
    cmake_build wheelbri_shim   # builds dinput8.dll
    shim=$shim_x64
fi
[ -f "$shim" ] || fail "no $shim after the build"

if [ -e "$dll" ] && ! cmp -s "$shim" "$dll" && [ ! -e "$ini" ]; then
    fail "$dll belongs to another mod (not WheelBri's): left alone, nothing put in; remove it first"
fi
if cmp -s "$shim" "$dll" 2>/dev/null; then
    ok "the $bits-bit shim is already in $game (as $name)"
else
    cp -f "$shim" "$dll" || fail "could not copy (is the game running?)"
    ok "copied the $bits-bit shim to $game (as $name)"
fi
if [ ! -f "$ini" ]; then
    cp config/wheelbri-shim.ini "$ini" && ok "copied wheelbri-shim.ini (defaults)"
fi
