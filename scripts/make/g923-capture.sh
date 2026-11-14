#!/usr/bin/env bash
# Capture a real Logitech G923 (Xbox One and PC, 046D:C26E) with USBPcap, guided, and decode it: the TrueForce
# parameters, status layout, HID++ answers and descriptors WheelBri's virtual G923 needs. Windows only (MSYS2 UCRT64).
# Plan: "Claude outputs/plans/g923-capture-automation-plan.md"; run sheet: tools/g923-capture/README.md.
#
#   scripts/make/g923-capture.sh check              is the PC ready (read-only); finds the wheel's USB hub and address
#   scripts/make/g923-capture.sh session S1         plug-in + G HUB          (Administrator window)
#   scripts/make/g923-capture.sh session S2         ACC with TrueForce       (puts the logging-only shim into ACC)
#   scripts/make/g923-capture.sh probe              read-only questions to the wheel (run with G HUB open, then closed)
#   scripts/make/g923-capture.sh session S3         ACC with G HUB closed
#   scripts/make/g923-capture.sh session S4         DiRT Rally 2.0 (skips itself if it has no TrueForce SDK)
#   scripts/make/g923-capture.sh session S5         NFS Heat, DirectInput baseline
#   scripts/make/g923-capture.sh decode [folder]    decode again (default: today's folder) -> report.md
#   scripts/make/g923-capture.sh restore            shim and its ini back as they were, G HUB's service, USBPcap removal
#   scripts/make/g923-capture.sh check --wheelbri   afterwards: the R3 back, nothing of the capture left
#   scripts/make/g923-capture.sh test               the kit's own tests (no wheel needed)
#
# Decoder and probe: wheelbri-g923cap (tools/g923-capture), built incrementally by each command that uses it.
# Captures go to captures/g923/<date>/ (git-ignored); G923_DAY=<folder name> picks another day's folder.
#
# Rehearsal with another wheel (the MOZA R3), to try the whole flow before the G923 is there:
#   G923_TARGET=346E scripts/make/g923-capture.sh check | session S1 | session S2 | decode | restore
# VID[:PID] in hex. WheelBri must still be stopped; the R3 and its own software may stay. TrueForce and HID++ checks are
# left out; captures go to captures/g923/rehearsal-<date>/.

# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

kit="$root/tools/g923-capture"
target=${G923_TARGET:-}

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ $# -ge 1 ] || usage
command=$1
shift

# The captures folder: `check` starts a day (today's date) and remembers it, so sessions, decode and restore after
# midnight, or the next morning, still use the same folder. G923_DAY=<name> picks one by hand.
if [ -n "$target" ]; then marker="$root/captures/g923/.current-rehearsal"; prefix=rehearsal-; else marker="$root/captures/g923/.current"; prefix=; fi
if [ -n "${G923_DAY:-}" ]; then
    day_name=$G923_DAY
elif [ "$command" = check ] && [ "${1:-}" != "--wheelbri" ]; then
    day_name="$prefix$(date +%F)"
    mkdir -p "$root/captures/g923" && echo "$day_name" >"$marker"
elif [ -f "$marker" ]; then
    day_name=$(tr -d '\r\n' <"$marker")
else
    day_name="$prefix$(date +%F)"
fi
day="$root/captures/g923/$day_name"
case "$command" in check|session|probe|decode|restore) say "captures folder: captures/g923/$day_name" ;; esac

# The decoder and probe, wheelbri-g923cap: built (incrementally) before each use, so it always matches the source.
tool_unix=""
tool_win=""
need_tool() {
    cmake_build wheelbri-g923cap
    tool_unix="$build_dir/tools/g923-capture/wheelbri-g923cap"
    on_windows && tool_unix="$tool_unix.exe"
    [ -x "$tool_unix" ] || fail "no $tool_unix after the build"
    if on_windows; then tool_win=$(cygpath -w "$tool_unix"); else tool_win=$tool_unix; fi
}

run_ps() {
    on_windows || fail "the capture is Windows only (MSYS2 UCRT64 shell)"
    mkdir -p "$day"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$kit/g923-capture.ps1")" \
        -Repo "$(cygpath -w "$root")" -Day "$(cygpath -w "$day")" ${tool_win:+-Tool "$tool_win"} ${target:+-Target "$target"} "$@"
}

# name=exe folder lines from the PowerShell part (Steam's libraries), for acc, dirt and nfsheat.
game_folder() {
    # Not found, or the PowerShell part failed: nothing (the caller says so).
    run_ps -Step games | tr -d '\r' | sed -n "s/^$1=//p" | head -n 1 || true
}

shim_state="$day/shim-state.txt"

# The WheelBri shim into a game's exe folder, logging only (emulate = false, logi_log = true). What was there is
# recorded once, so restore puts it back.
shim_on() {
    local key=$1 win folder ini had_dll had_ini
    win=$(game_folder "$key")
    if [ -z "$win" ]; then warn "$key not found in Steam's libraries: no shim"; return 0; fi
    folder=$(cygpath -u "$win")
    ini="$folder/wheelbri-shim.ini"
    mkdir -p "$day"
    if ! grep -qF "|$folder|" "$shim_state" 2>/dev/null; then
        had_dll=0; { [ -f "$folder/dinput8.dll" ] || [ -f "$folder/dinput8Hooked.dll" ]; } && had_dll=1
        had_ini=0; [ -f "$ini" ] && had_ini=1
        if [ $had_ini = 1 ] && [ ! -f "$ini.before-g923" ]; then cp "$ini" "$ini.before-g923" || fail "could not back up $ini"; fi
        # A dinput8.dll already there (an older shim, another mod) is kept aside, so restore can put it back.
        if [ -f "$folder/dinput8.dll" ] && [ ! -f "$folder/dinput8.dll.before-g923" ]; then
            cp "$folder/dinput8.dll" "$folder/dinput8.dll.before-g923" || fail "could not back up $folder/dinput8.dll"
        fi
        echo "$key|$folder|$had_dll|$had_ini" >>"$shim_state"
    fi
    bash "$root/scripts/make/shim.sh" "$win" || { warn "the shim did not go into $key: its SDK calls will not be logged"; return 0; }
    if grep -q '^[[:space:]]*emulate[[:space:]]*=' "$ini"; then
        sed -i 's/^\([[:space:]]*emulate[[:space:]]*=[[:space:]]*\)[A-Za-z]*/\1false/' "$ini"
    else
        echo 'emulate = false' >>"$ini"
    fi
    if grep -q '^[[:space:]]*logi_log[[:space:]]*=' "$ini"; then
        sed -i 's/^\([[:space:]]*logi_log[[:space:]]*=[[:space:]]*\)[A-Za-z]*/\1true/' "$ini"
    else
        echo 'logi_log = true' >>"$ini"
    fi
    local emulate logi
    emulate=$(grep -i '^[[:space:]]*emulate[[:space:]]*=' "$ini" | tail -n 1 | sed 's/[;#].*//; s/.*=[[:space:]]*//' | tr -d '\r ') || true
    logi=$(grep -i '^[[:space:]]*logi_log[[:space:]]*=' "$ini" | tail -n 1 | sed 's/[;#].*//; s/.*=[[:space:]]*//' | tr -d '\r ') || true
    [ "$emulate" = false ] && [ "$logi" = true ] || fail "$ini reads emulate=$emulate logi_log=$logi, not false/true"
    ok "shim in $key, logging only (emulate = false, logi_log = true): $ini"
}

shim_restore() {
    [ -f "$shim_state" ] || { say "no shim was put in by the capture"; return 0; }
    local key folder had_dll had_ini ini failed=0
    while IFS='|' read -r key folder had_dll had_ini; do
        [ -n "$folder" ] || continue
        ini="$folder/wheelbri-shim.ini"
        if [ -f "$folder/dinput8.dll.before-g923" ]; then
            if mv -f "$folder/dinput8.dll.before-g923" "$folder/dinput8.dll"; then ok "$key: the dinput8.dll that was there is back"
            else warn "$key: could not put the old dinput8.dll back (is the game running?)"; failed=1; fi
        elif [ "$had_dll" = 0 ] && [ -f "$folder/dinput8.dll" ]; then
            bash "$root/scripts/make/shim.sh" --off "$(cygpath -w "$folder")" || { warn "could not take the shim out of $key (is the game running?)"; failed=1; }
        fi
        if [ -f "$ini.before-g923" ]; then
            if mv -f "$ini.before-g923" "$ini"; then ok "$key: wheelbri-shim.ini back as it was"; else failed=1; fi
        elif [ "$had_ini" = 0 ] && [ -f "$ini" ]; then
            if mv -f "$ini" "$ini.g923"; then ok "$key: the capture's wheelbri-shim.ini renamed to wheelbri-shim.ini.g923"; else failed=1; fi
        fi
    done <"$shim_state"
    if [ $failed = 0 ]; then mv -f "$shim_state" "$shim_state.done"
    else warn "not everything was put back: close the game and run restore again"; fi
}

# The shim logs at the level in WheelBri's app.ini ([app] log_level, read when the game starts; warn by default), and
# logi_log's lines are info: without this the shim log stays empty. Set to debug (G923_SHIM_LEVEL=trace for more) for
# the game sessions; restore puts the file back as it was.
shim_level=${G923_SHIM_LEVEL:-debug}
case "$shim_level" in info|debug|trace) ;; *) fail "G923_SHIM_LEVEL must be info, debug or trace" ;; esac
app_ini() { [ -n "${LOCALAPPDATA:-}" ] && echo "$(cygpath -u "$LOCALAPPDATA")/wheelbri/app.ini"; }
applog_state="$day/applog-state.txt"

applog_on() {
    local ini
    ini=$(app_ini) || { warn "no LOCALAPPDATA: the shim log level is not changed"; return 0; }
    mkdir -p "$day" "$(dirname "$ini")"
    if [ ! -f "$applog_state" ]; then
        if [ -f "$ini" ]; then cp -f "$ini" "$ini.before-g923" && echo "had=1" >"$applog_state"
        else echo "had=0" >"$applog_state"; fi
    fi
    if [ -f "$ini" ] && grep -qi '^[[:space:]]*log_level[[:space:]]*=' "$ini"; then
        sed -i "s/^\([[:space:]]*log_level[[:space:]]*=[[:space:]]*\).*$/\1$shim_level/I" "$ini"
    elif [ -f "$ini" ] && grep -qi '^[[:space:]]*\[app\]' "$ini"; then
        sed -i "0,/^[[:space:]]*\[[Aa][Pp][Pp]\]/s//&\nlog_level = $shim_level/" "$ini"
    else
        printf '\n[app]\nlog_level = %s\n' "$shim_level" >>"$ini"
    fi
    # Read back as the shim reads it: the last log_level line wins.
    local now
    now=$(grep -i '^[[:space:]]*log_level[[:space:]]*=' "$ini" | tail -n 1 | sed 's/.*=[[:space:]]*//' | tr -d '\r ') || true
    [ "$now" = "$shim_level" ] || fail "app.ini's log_level reads '$now', not $shim_level: $ini"
    ok "shim log level $shim_level for the capture (app.ini [app] log_level): $ini"
}

applog_restore() {
    [ -f "$applog_state" ] || return 0
    local ini
    ini=$(app_ini) || return 0
    if grep -q '^had=1' "$applog_state"; then
        if [ -f "$ini.before-g923" ]; then
            mv -f "$ini.before-g923" "$ini" && ok "app.ini back as it was (log level)" || { warn "could not put app.ini back (is WheelBri running?)"; return 0; }
        fi
    else
        if [ -f "$ini" ]; then
            mv -f "$ini" "$ini.g923" && ok "the capture's app.ini renamed to app.ini.g923 (there was none before)" || { warn "could not move app.ini"; return 0; }
        fi
    fi
    mv -f "$applog_state" "$applog_state.done"
}

print_summary() {
    local last
    [ -f "$day/last-session.txt" ] || return 0
    last=$(tr -d '\r' <"$day/last-session.txt")
    [ -n "$last" ] || return 0
    last=$(cygpath -u "$last")
    if [ -f "$last/summary.md" ]; then
        say "---- $last/summary.md (top)"
        sed -n '1,25p' "$last/summary.md"
    fi
}

case "$command" in
    check)
        need_tool
        if [ "${1:-}" = "--wheelbri" ]; then run_ps -Step check -WheelBri; else run_ps -Step check; fi
        if [ -f "$day/S0-check/check.md" ]; then say "saved: $day/S0-check/check.md"; fi
        ;;
    session)
        [ $# -eq 1 ] || fail "usage: g923-capture.sh session S1|S2|S3|S4|S5"
        s=$(echo "$1" | tr '[:lower:]' '[:upper:]')
        need_tool
        case "$s" in
            S2|S3) shim_on acc; applog_on ;;
            S4) shim_on dirt; applog_on ;;
            S1|S5) ;;
            *) fail "no session $1 (S1..S5)" ;;
        esac
        run_ps -Step session -Session "$s"
        print_summary
        ;;
    probe)
        need_tool
        on_windows || fail "the probe is Windows only"
        ghub=closed
        # grep without -q reads the whole list: tasklist is never cut off, so pipefail sees its own status.
        if tasklist.exe 2>/dev/null | grep -iE '^lghub(_agent|_system_tray)?\.exe' >/dev/null; then ghub=running; fi
        say "probing the G923 (G HUB $ghub); read-only requests only"
        "$tool_unix" probe --out "$(cygpath -w "$day/S6-probe")" --ghub "$ghub" \
            || fail "the probe failed (see above)"
        if [ $ghub = running ]; then say "now close G HUB fully (tray > Quit) and run probe once more"; fi
        ;;
    decode)
        need_tool
        folder=${1:-$day}
        [ -d "$folder" ] || fail "no folder $folder"
        status=0
        if on_windows; then "$tool_unix" decode "$(cygpath -w "$folder")" || status=$?
        else "$tool_unix" decode "$folder" || status=$?; fi
        if [ -f "$folder/report.md" ]; then say "report: $folder/report.md"; fi
        exit "$status"
        ;;
    restore)
        shim_restore
        applog_restore
        run_ps -Step restore
        ;;
    test)
        cmake_build wheelbri_tests
        tests="$build_dir/tests/wheelbri_tests"
        on_windows && tests="$tests.exe"
        "$tests" --gtest_filter='G923Cap*'
        ;;
    *) usage ;;
esac
