#!/usr/bin/env bash
# Linux / Steam Deck: starts everything for a game with your rig as a G920, and stops it again.
#
# Two ways for the game to see the G920:
#   uhid (default)  wheelbri-uhid makes a virtual G920 for the whole system (Phase 4); the game reads it raw
#                   (PROTON_ENABLE_HIDRAW) and the rig is hidden from it (SDL_GAMECONTROLLER_IGNORE_DEVICES).
#   --shim          the dinput8.dll proxy in the game folder answers as a G920 (copy dinput8.dll there first);
#                   WINEDLLOVERRIDES makes Proton load it.
#
# As a Steam launch option (works in Game Mode; the game's own exit stops everything):
#   /home/deck/wheelbri/wheelbri.sh %command%
#   /home/deck/wheelbri/wheelbri.sh --shim %command%
# From a terminal (Desktop Mode), without a game: starts, waits for Enter or Ctrl+C, stops.
#   ./wheelbri.sh            (service with its --ui window)
#
# Options: --shim, --ui / --no-ui (default: window only from a terminal), --config FILE,
#          --hide VID/PID[,VID/PID...] (the devices to hide from the game; default: every device of the active rig,
#          set up in the Rig tab of wheelbri-service --ui).
# Once before the first use: install-udev-rules.sh (needs sudo). Logs: ~/.local/state/wheelbri.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# The packaged bundle (bin/ next to this script) or the source tree (scripts/linux/ in it, built in build/).
if [ -x "$here/bin/wheelbri-service" ]; then
    service="$here/bin/wheelbri-service"
    uhid="$here/bin/wheelbri-uhid"
    config="$here/config/rig-as-g920.ini"
else
    root=$(cd "$here/../.." && pwd)
    service="$root/build/service/wheelbri-service"
    uhid="$root/build/tools/uhid/wheelbri-uhid"
    config="$root/config/rig-as-g920.ini"
fi

mode=uhid
ui=
hide=   # empty: the active rig's devices (below)
game=()
while [ $# -gt 0 ]; do
    case "$1" in
        --shim) mode=shim ;;
        --ui) ui=1 ;;
        --no-ui) ui=0 ;;
        --config) shift; config=${1:-} ;;
        --hide) shift; hide=${1:-} ;;
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; game=("$@"); break ;;
        -*) echo "wheelbri.sh: unknown option $1 (see --help)" >&2; exit 2 ;;
        *) game=("$@"); break ;;          # Steam's %command%
    esac
    shift
done
[ -z "$ui" ] && { [ ${#game[@]} -eq 0 ] && ui=1 || ui=0; }

# Default --hide: every device of the active rig (app.ini [app] rig, then id = vvvv:pppp in rigs/<rig>.ini).
if [ -z "$hide" ]; then
    settings="${XDG_CONFIG_HOME:-$HOME/.config}/wheelbri"
    rig=$(awk -F= '/^\[/{app = ($0 ~ /^\[app\]/); next} app && $1 ~ /^[ \t]*rig[ \t]*$/ {sub(/[;#].*/, "", $2); gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}' \
          "$settings/app.ini" 2>/dev/null)
    if [ -n "$rig" ] && [ -f "$settings/rigs/$rig.ini" ]; then
        hide=$(awk -F= '$1 ~ /^[ \t]*id[ \t]*$/ {sub(/[;#].*/, "", $2); gsub(/[ \t]/, "", $2); split($2, p, ":");
                        if (length(p[1]) == 4 && length(p[2]) == 4 && toupper($2) != "046D:C262")
                            printf "%s0x%s/0x%s", (n++ ? "," : ""), toupper(p[1]), toupper(p[2])}' "$settings/rigs/$rig.ini")
    fi
    [ -z "$hide" ] && echo "wheelbri.sh: no rig yet (set it up in the Rig tab of wheelbri-service --ui): nothing is hidden from the game" >&2
fi

state="${XDG_STATE_HOME:-$HOME/.local/state}/wheelbri"
mkdir -p "$state"
service_stop="$state/service.stop"
uhid_stop="$state/uhid.stop"
log="$state/wheelbri-sh.log"

say() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$log" >&2; }

[ -x "$service" ] || { say "missing $service (build it, or unpack the bundle next to this script)"; exit 1; }
[ "$mode" = shim ] || [ -x "$uhid" ] || { say "missing $uhid"; exit 1; }

# Our programs are native: none of Steam's preloads (overlay) or runtime libraries.
clean_env=(env -u LD_PRELOAD -u LD_LIBRARY_PATH)

service_pid=
uhid_pid=
own_service=0

start() {
    rm -f "$service_stop" "$uhid_stop"
    if pgrep -f '(^|/)wheelbri-service( |$)' >/dev/null; then
        say "wheelbri-service is already running: using it (and leaving it running afterwards)"
    else
        local args=(--config "$config" --stop-file "$service_stop")
        [ "$ui" = 1 ] && args+=(--ui)
        "${clean_env[@]}" "$service" "${args[@]}" >>"$state/wheelbri-service.console" 2>&1 &
        service_pid=$!
        own_service=1
        sleep 1.5
        if ! kill -0 "$service_pid" 2>/dev/null; then
            say "wheelbri-service stopped at once: see $state/wheelbri-service-*.log"
            return 1
        fi
        say "wheelbri-service started ($config)"
    fi

    if [ "$mode" = uhid ]; then
        if pgrep -f '(^|/)wheelbri-uhid( |$)' >/dev/null; then
            say "wheelbri-uhid is already running: using it"
        else
            "${clean_env[@]}" "$uhid" --stop-file "$uhid_stop" >>"$state/wheelbri-uhid.console" 2>&1 &
            uhid_pid=$!
            sleep 1
            if ! kill -0 "$uhid_pid" 2>/dev/null; then
                say "wheelbri-uhid stopped at once: see $state/wheelbri-uhid-*.log (install-udev-rules.sh run?)"
                return 1
            fi
            say "virtual G920 created (wheelbri-uhid)"
        fi
        # Raw HID for the G920 (its force feedback is HID PID), and the real wheel hidden from the game.
        export PROTON_ENABLE_HIDRAW="${PROTON_ENABLE_HIDRAW:+$PROTON_ENABLE_HIDRAW,}0x046D/0xC262"
        export SDL_GAMECONTROLLER_IGNORE_DEVICES="${SDL_GAMECONTROLLER_IGNORE_DEVICES:+$SDL_GAMECONTROLLER_IGNORE_DEVICES,}$hide"
    else
        export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}dinput8=n,b"
    fi
    return 0
}

# Waits up to $2 seconds for pid $1, then asks it with SIGTERM (the same clean stop).
wait_for() {
    local pid=$1 seconds=$2 i
    for ((i = 0; i < seconds * 4; ++i)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.25
    done
    kill -TERM "$pid" 2>/dev/null
    for ((i = 0; i < 20; ++i)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.25
    done
    say "pid $pid did not stop"
}

stopped=0
stop() {
    [ $stopped = 1 ] && return
    stopped=1
    trap '' INT TERM
    if [ -n "$uhid_pid" ]; then
        touch "$uhid_stop"
        wait_for "$uhid_pid" 5
        say "virtual G920 removed"
    fi
    if [ $own_service = 1 ] && [ -n "$service_pid" ]; then
        touch "$service_stop"
        wait_for "$service_pid" 10
        say "wheelbri-service stopped"
    fi
    rm -f "$service_stop" "$uhid_stop"
}
trap 'stop; exit 130' INT
trap 'stop; exit 143' TERM

if ! start; then
    stop
    exit 1
fi

if [ ${#game[@]} -gt 0 ]; then
    say "starting the game ($mode)"
    "${game[@]}"
    status=$?
    say "game ended ($status)"
    stop
    exit $status
fi

say "running ($mode). Start the game$([ "$mode" = uhid ] && echo " with the launch option: PROTON_ENABLE_HIDRAW=0x046D/0xC262 SDL_GAMECONTROLLER_IGNORE_DEVICES=$hide %command%" || echo " with: WINEDLLOVERRIDES=\"dinput8=n,b\" %command%")"
say "press Enter (or Ctrl+C) here to stop everything"
while :; do
    read -r -t 2
    rc=$?
    [ $rc -eq 0 ] && break
    [ $rc -le 128 ] && sleep 2      # no keyboard (input closed): keep watching
    if [ -n "$uhid_pid" ] && ! kill -0 "$uhid_pid" 2>/dev/null; then say "wheelbri-uhid stopped"; break; fi
    if [ $own_service = 1 ] && ! kill -0 "$service_pid" 2>/dev/null; then say "wheelbri-service stopped"; break; fi
done
stop
