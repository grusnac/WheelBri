#!/usr/bin/env bash
# Linux / Steam Deck: starts everything for a game with the Moza R3 as a G920, and stops it again.
#
# Two ways for the game to see the G920:
#   uhid (default)  wheelemu-uhid makes a virtual G920 for the whole system (Phase 4); the game reads it raw
#                   (PROTON_ENABLE_HIDRAW) and the R3 is hidden from it (SDL_GAMECONTROLLER_IGNORE_DEVICES).
#   --shim          the dinput8.dll proxy in the game folder answers as a G920 (copy dinput8.dll there first);
#                   WINEDLLOVERRIDES makes Proton load it.
#
# As a Steam launch option (works in Game Mode; the game's own exit stops everything):
#   /home/deck/wheelemu/wheelemu.sh %command%
#   /home/deck/wheelemu/wheelemu.sh --shim %command%
# From a terminal (Desktop Mode), without a game: starts, waits for Enter or Ctrl+C, stops.
#   ./wheelemu.sh            (service with its --ui window)
#
# Options: --shim, --ui / --no-ui (default: window only from a terminal), --config FILE,
#          --hide VID/PID (the real wheel to hide from the game, default 0x346E/0x0005 = Moza R3).
# Once before the first use: install-udev-rules.sh (needs sudo). Logs: ~/.local/state/wheelemu.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# The packaged bundle (bin/ next to this script) or the source tree (scripts/linux/ in it, built in build/).
if [ -x "$here/bin/wheelemu-service" ]; then
    service="$here/bin/wheelemu-service"
    uhid="$here/bin/wheelemu-uhid"
    config="$here/config/moza-r3-as-g920.ini"
else
    root=$(cd "$here/../.." && pwd)
    service="$root/build/service/wheelemu-service"
    uhid="$root/build/tools/uhid/wheelemu-uhid"
    config="$root/config/moza-r3-as-g920.ini"
fi

mode=uhid
ui=
hide="0x346E/0x0005"
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
        -*) echo "wheelemu.sh: unknown option $1 (see --help)" >&2; exit 2 ;;
        *) game=("$@"); break ;;          # Steam's %command%
    esac
    shift
done
[ -z "$ui" ] && { [ ${#game[@]} -eq 0 ] && ui=1 || ui=0; }

state="${XDG_STATE_HOME:-$HOME/.local/state}/wheelemu"
mkdir -p "$state"
service_stop="$state/service.stop"
uhid_stop="$state/uhid.stop"
log="$state/wheelemu-sh.log"

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
    if pgrep -f '(^|/)wheelemu-service( |$)' >/dev/null; then
        say "wheelemu-service is already running: using it (and leaving it running afterwards)"
    else
        local args=(--config "$config" --stop-file "$service_stop")
        [ "$ui" = 1 ] && args+=(--ui)
        "${clean_env[@]}" "$service" "${args[@]}" >>"$state/wheelemu-service.console" 2>&1 &
        service_pid=$!
        own_service=1
        sleep 1.5
        if ! kill -0 "$service_pid" 2>/dev/null; then
            say "wheelemu-service stopped at once: see $state/wheelemu-service-*.log"
            return 1
        fi
        say "wheelemu-service started ($config)"
    fi

    if [ "$mode" = uhid ]; then
        if pgrep -f '(^|/)wheelemu-uhid( |$)' >/dev/null; then
            say "wheelemu-uhid is already running: using it"
        else
            "${clean_env[@]}" "$uhid" --stop-file "$uhid_stop" >>"$state/wheelemu-uhid.console" 2>&1 &
            uhid_pid=$!
            sleep 1
            if ! kill -0 "$uhid_pid" 2>/dev/null; then
                say "wheelemu-uhid stopped at once: see $state/wheelemu-uhid-*.log (install-udev-rules.sh run?)"
                return 1
            fi
            say "virtual G920 created (wheelemu-uhid)"
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
        say "wheelemu-service stopped"
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
    if [ -n "$uhid_pid" ] && ! kill -0 "$uhid_pid" 2>/dev/null; then say "wheelemu-uhid stopped"; break; fi
    if [ $own_service = 1 ] && ! kill -0 "$service_pid" 2>/dev/null; then say "wheelemu-service stopped"; break; fi
done
stop
