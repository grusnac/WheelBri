#!/usr/bin/env bash
# Build wheelbri-logi.dll, the logging stand-in for Logitech's wheel SDK helper DLL (logi/), and print the
# Administrator PowerShell commands that point the SDK's registry entry at it or back at G HUB's library.
# Games built with Logitech's wheel SDK (NFS Payback) load whatever DLL that entry names, when they start.
#
#   scripts/make/logi.sh

source "$(dirname "$0")/common.sh"

on_windows || fail "wheelbri-logi.dll is a Windows DLL: build it on Windows, or cross-compile (README, Build)"
cmake_build wheelbri_logi
dll=$(cygpath -w "$root/$build_dir/logi/wheelbri-logi.dll")
[ -f "$root/$build_dir/logi/wheelbri-logi.dll" ] || fail "no $build_dir/logi/wheelbri-logi.dll after the build"
ok "built $dll"

key='HKLM\SOFTWARE\Classes\CLSID\{63BD165D-1584-4E75-AB56-08330350545F}\ServerBinary'
ghub='C:\Program Files\Logi\wheel_sdk\9_1_1\logi_steering_wheel_x64.dll'
say "Point Logitech's wheel SDK at the stand-in (Administrator PowerShell, before starting the game):"
printf '    reg add "%s" /ve /t REG_SZ /d "%s" /f\n' "$key" "$dll"
say "Back to G HUB's library afterwards:"
printf '    reg add "%s" /ve /t REG_SZ /d "%s" /f\n' "$key" "$ghub"
say "Its log: %LOCALAPPDATA%\\wheelbri\\logs\\wheelbri-logi-<date>-<NN>.log (set the app's Logs level to Debug first)."
