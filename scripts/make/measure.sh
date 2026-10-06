#!/usr/bin/env bash
# Measure what WheelBri costs the PC (CPU per process and per thread, memory, handles, GDI objects, the timer
# resolution), from outside the app: runs measure.ps1 in its own PowerShell window as Administrator (the helper's and
# the driver hosts' figures need it; the keys M = new phase and Q = stop need a real console window). The window stays
# open at the end with the summary; everything goes to one log:
# %LOCALAPPDATA%\wheelbri\logs\perf-<date>-<time>.log.
#
#   scripts/make/measure.sh                                 until Q
#   scripts/make/measure.sh -Phase stopped                  name the first phase
#   scripts/make/measure.sh -Game NFS16                     also measure the game
#   scripts/make/measure.sh -Seconds 300                    stop by itself after 5 minutes

source "$(dirname "$0")/common.sh"

on_windows || fail "measure is Windows only (MSYS2 UCRT64 shell)"
arguments="-NoProfile -ExecutionPolicy Bypass -NoExit -File \"$(cygpath -w "$root/scripts/make/measure.ps1")\""
for arg in "$@"; do
    arguments+=" \"${arg//\"/}\""   # each argument quoted (a " inside cannot be passed this way: dropped)
done
say "opening the measurement in its own PowerShell window (answer the Administrator prompt)"
powershell.exe -NoProfile -Command "Start-Process powershell.exe -Verb RunAs -ArgumentList $(ps_quote "$arguments")" \
    || fail "the measurement window did not open (Administrator refused?)"
