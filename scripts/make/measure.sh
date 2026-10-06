#!/usr/bin/env bash
# Measure what WheelBri costs the PC (CPU per process and per thread, memory, handles, GDI objects, the timer
# resolution), from outside the app: runs measure.ps1 in its own PowerShell window as Administrator (the helper's and
# the driver hosts' figures need it; the keys M = new phase and Q = stop need a real console window). That window closes
# when it ends (Q or -Seconds) and the summary is shown here; everything goes to one log:
# %LOCALAPPDATA%\wheelbri\logs\perf-<date>-<time>.log.
#
#   scripts/make/measure.sh                                 until Q
#   scripts/make/measure.sh -Phase stopped                  name the first phase
#   scripts/make/measure.sh -Game NFS16                     also measure the game
#   scripts/make/measure.sh -Seconds 300                    stop by itself after 5 minutes

source "$(dirname "$0")/common.sh"

on_windows || fail "measure is Windows only (MSYS2 UCRT64 shell)"
arguments="-NoProfile -ExecutionPolicy Bypass -File \"$(cygpath -w "$root/scripts/make/measure.ps1")\""
for arg in "$@"; do
    arguments+=" \"${arg//\"/}\""   # each argument quoted (a " inside cannot be passed this way: dropped)
done
say "opening the measurement in its own PowerShell window (answer the Administrator prompt)"
logs="$(cygpath "$LOCALAPPDATA")/wheelbri/logs"
before=$(ls -t "$logs"/perf-*.log 2>/dev/null | head -n 1)
powershell.exe -NoProfile -Command "Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList $(ps_quote "$arguments")" \
    || fail "the measurement window did not open (Administrator refused?)"
# The summary, from the log the window wrote (its messages without the date and component).
latest=$(ls -t "$logs"/perf-*.log 2>/dev/null | head -n 1)
[ -n "$latest" ] && [ "$latest" != "$before" ] || fail "the measurement wrote no log in $logs"
grep -F '[perf.summary' "$latest" | sed 's/^\[[^]]*\] \[[^]]*\] \[[^]]*\] //'
ok "log: $(cygpath -w "$latest")"
