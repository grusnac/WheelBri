#!/usr/bin/env bash
# Find what a question needs in a folder of WheelBri logs, never the logs whole (bash and POSIX awk only: MSYS2
# UCRT64 on Windows, any Linux shell). Every line printed is "<log> <time> <message>", the same message is shown once
# with a count, and at most --max lines are printed (default 40).
#
# usage: scripts/run_summary.sh [FOLDER] [--find TOPIC]... [--grep REGEX] [--in service|shim|all] [--max N]
#
#   (nothing)       an index: one line per Start (time, config, pedals, games) and per game launch (exe, profile)
#   --find pedals   pedal mode and where it came from (service), the profile's pedals and the values the game saw (shim)
#   --find game     games found, started and ended; the profile each one used
#   --find input    what the game asked DirectInput for: devices, data formats, axis ranges, dead zones, action maps
#   --find state    sampled GetDeviceState of the emulated wheel (what the game read)
#   --find ffb      the game's force feedback: effects made, held forces, quiet times
#   --find problems warnings and errors
#   --grep REGEX    any message matching REGEX (awk regex), in the logs --in names (default all)
#
# FOLDER: a copy of the log folder (e.g. one brought from a test PC) or, by default, the app's own:
# %LOCALAPPDATA%\wheelbri\logs on Windows, ${XDG_STATE_HOME:-$HOME/.local/state}/wheelbri on Linux.
# Telemetry (axes over time): telemetry_summary.sh.

set -euo pipefail

folder=""
max=40
in="all"
topics=()
regex=""
while [ $# -gt 0 ]; do
    case "$1" in
        --find) topics+=("${2:?--find needs a topic}"); shift 2 ;;
        --grep) regex="${2:?--grep needs a regex}"; shift 2 ;;
        --in) in="${2:?--in needs service, shim or all}"; shift 2 ;;
        --max) max="${2:?--max needs a number}"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "run_summary: unknown option $1 (--help)" >&2; exit 2 ;;
        *) folder="$1"; shift ;;
    esac
done
if [ -z "$folder" ]; then
    if [ -n "${LOCALAPPDATA:-}" ]; then
        folder="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || printf '%s' "$LOCALAPPDATA")/wheelbri/logs"
    else
        folder="${XDG_STATE_HOME:-$HOME/.local/state}/wheelbri"
    fi
fi
[ -d "$folder" ] || { echo "run_summary: no folder $folder" >&2; exit 1; }

shopt -s nullglob
services=("$folder"/wheelbri-service-*.log)
shims=("$folder"/wheelbri-shim-*.log)
case "$in" in
    service) files=("${services[@]}") ;;
    shim) files=("${shims[@]}") ;;
    all) files=("${services[@]}" "${shims[@]}") ;;
    *) echo "run_summary: --in service, shim or all" >&2; exit 2 ;;
esac
[ ${#files[@]} -gt 0 ] || { echo "run_summary: no WheelBri logs in $folder" >&2; exit 1; }

# The patterns each topic looks for, as one awk regex over the message.
topicRegex() {
    case "$1" in
        pedals) echo '^pedals: |pedals as the game sees them|^game profile .*(combined|axes now)|^buttons: ' ;;
        game) echo '^game: |^game profile |dinput8 proxy loaded into|^Games tab: |shim: ready in' ;;
        input) echo 'EnumDevices\(|CreateDevice\(|first SetDataFormat|DIPROP_(RANGE|DEADZONE|SATURATION|AXISMODE)|BuildActionMap|SetActionMap|EnumDevicesBySemantics|not emulated|presenting .* to the game as|SetCooperativeLevel\(' ;;
        state) echo '\]: GetDeviceState #|GetDeviceData #' ;;
        ffb) echo 'CreateEffect\(|^ffb: |held a [0-9]+% push|ffb usage: +[a-z]' ;;
        problems) echo '' ;;
        *) echo "run_summary: unknown topic $1 (--help)" >&2; exit 2 ;;
    esac
}

# Prints matching lines as "<log> <time> <message>"; the same message (numbers aside) once, with a count.
# problems=1: warnings and errors instead of a regex. index=1: the index (Starts and launches) instead.
scan() {
    # The regex goes through the environment: awk -v would eat its backslashes.
    RUN_SUMMARY_RE="$1" awk -v problems="$2" -v index_="$3" -v max="$max" '
    BEGIN { re = ENVIRON["RUN_SUMMARY_RE"] }
    function msg(line) {
        sub(/^\[[^]]*\] \[[^]]*\] \[[^]]*\] \[[^]]*\] /, "", line)
        sub(/  \([A-Za-z0-9_.]+:[0-9]+\)$/, "", line)
        gsub(/\[G[0-9]+ emulated over [^]]*\]/, "[wheel]", line)
        return line
    }
    function short(file) { sub(/.*\//, "", file); sub(/^wheelbri-/, "", file); sub(/-[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]-/, "-", file); sub(/\.log$/, "", file); return file }
    function keep(text, key) {
        # The same message: problems whatever their numbers; other lines only when their values are the same.
        if (key == "") {
            key = substr(text, index(text, " ") + 10)
            if (problems) gsub(/[0-9]+/, "N", key); else gsub(/#[0-9]+|[0-9]+ ms|pid [0-9]+/, "N", key)
        }
        if (!(key in seen)) { order[++n] = key; line[key] = text }
        seen[key]++
    }
    FNR == 1 { f = short(FILENAME); started = 0 }
    {
        t = substr($0, 13, 8); m = msg($0); lvl = substr($0, 27, 5)
        if (index_) {
            if (m ~ /dinput8 proxy loaded into/) { e = m; sub(/.*loaded into /, "", e); keep(f " " t " game " e, f "g") }
            else if (m ~ /^game profile [a-z0-9_]+ \([^)]*\) laid over/) { p = m; sub(/ laid over.*/, "", p); keep(f " " t "   " p, f "p") }
            else if (m ~ /^config: /) { keep(f " " t " " m, f "c") }
            else if (m ~ /^Start pressed:/ && !started) { started = 1; keep(f " " t " " m, f "s") }
            else if (m ~ /^pedals: /) { p = m; sub(/: the .*|: throttle, .*/, "", p); keep(f " " t "   " p, f p) }
            else if (m ~ /^game: .* (started|ended)/) { g = m; sub(/ \(pid [0-9]+, [^)]*\)/, "", g); sub(/: game profile.*|: the defaults.*/, "", g); keep(f " " t "   " g, f g) }
            else if (m ~ /^Stop pressed/) { keep(f " " t "   Stop", f "x") }
            next
        }
        if (problems ? lvl ~ /warn|error/ : m ~ re) keep(f " " t " " m, "")
    }
    END {
        for (i = 1; i <= n && i <= max; i++) print (seen[order[i]] > 1 ? seen[order[i]] "x " : "") line[order[i]]
        if (n > max) print "... " n - max " more (--max, or a narrower --grep / --in)"
        if (n == 0) print "(nothing found)"
    }' "${files[@]}"
}

for topic in "${topics[@]}"; do topicRegex "$topic" > /dev/null; done   # an unknown topic stops here
if [ ${#topics[@]} -eq 0 ] && [ -z "$regex" ]; then
    scan "" 0 1
    exit 0
fi
for topic in "${topics[@]}"; do
    echo "== $topic"
    if [ "$topic" = problems ]; then scan "" 1 0; else scan "$(topicRegex "$topic")" 0 0; fi
done
if [ -n "$regex" ]; then
    echo "== grep $regex"
    scan "$regex" 0 0
fi
