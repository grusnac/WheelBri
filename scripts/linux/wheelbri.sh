#!/usr/bin/env bash
# Linux / Steam Deck: starts everything for a game with your rig as a G920, and stops it again.
#
# Two ways for the game to see the G920:
#   --virtual (default)  wheelbri-uhid makes a virtual G920 for the whole system (Phase 4); the game reads it raw
#                        (PROTON_ENABLE_HIDRAW) and the rig is hidden from it (SDL_GAMECONTROLLER_IGNORE_DEVICES).
#                        A WheelBri shim left in the game's folder (--keep-shim) is taken out first.
#   --shim               the dinput8.dll proxy answers as a G920 inside the game: dinput8.dll and wheelbri-shim.ini
#                        are put into the game's folder (the one with the .exe in %command%) before it starts and
#                        taken out when it ends; WINEDLLOVERRIDES makes Proton load it.
#
# As a Steam launch option (works in Game Mode; the game's own exit stops everything):
#   /home/deck/wheelbri/wheelbri.sh %command%
#   /home/deck/wheelbri/wheelbri.sh --shim %command%
# From a terminal (Desktop Mode), without a game: starts, waits for Enter or Ctrl+C, stops.
#   ./wheelbri.sh            (service with its --ui window)
#
# Options: --virtual / --shim, --keep-shim (leave the shim in the game's folder afterwards, for testing),
#          --game-folder DIR (where --shim puts it; default: the folder of the .exe in %command%),
#          --ui / --no-ui (default: window only from a terminal), --config FILE (default: the mapping saved in
#          the app's Mapping tab, else WheelBri's default for the target),
#          --hide VID/PID[,VID/PID...] (the devices to hide from the game; default: every device of the active rig,
#          set up in the Rig tab of wheelbri-service --ui).
# Once before the first use: install-udev-rules.sh (needs sudo). Logs: ~/.local/state/wheelbri.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# The packaged bundle (bin/ next to this script) or the source tree (scripts/linux/ in it, built in build/ or
# build-deck/).
if [ -x "$here/bin/wheelbri-service" ]; then
    service="$here/bin/wheelbri-service"
    uhid="$here/bin/wheelbri-uhid"
    configs="$here/config"
    shim_dlls=("$here/shim/dinput8.dll" "$here/dinput8.dll")
else
    root=$(cd "$here/../.." && pwd)
    build="$root/build"   # the Steam Deck's deck-build.sh builds in build-deck/
    [ -x "$build/service/wheelbri-service" ] || [ ! -x "$root/build-deck/service/wheelbri-service" ] || build="$root/build-deck"
    service="$build/service/wheelbri-service"
    uhid="$build/tools/uhid/wheelbri-uhid"
    configs="$root/config"
    shim_dlls=("$root/build-deck-shim/shim/dinput8.dll" "$root/build-win/shim/dinput8.dll")
fi

mode=uhid
config=   # empty: the target's built-in config (below)
ui=
hide=   # empty: the active rig's devices (below)
game=()
keep_shim=0
game_folder=   # empty: the folder of the .exe in the game's command (below)
while [ $# -gt 0 ]; do
    case "$1" in
        --virtual) mode=uhid ;;
        --shim) mode=shim ;;
        --keep-shim) keep_shim=1 ;;
        --game-folder) shift; game_folder=${1:-} ;;
        --ui) ui=1 ;;
        --no-ui) ui=0 ;;
        --config) shift; config=${1:-} ;;
        --hide) shift; hide=${1:-} ;;
        -h|--help) sed -n '2,/^# Once before/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; game=("$@"); break ;;
        -*) echo "wheelbri.sh: unknown option $1 (see --help)" >&2; exit 2 ;;
        *) game=("$@"); break ;;          # Steam's %command%
    esac
    shift
done
[ -z "$ui" ] && { [ ${#game[@]} -eq 0 ] && ui=1 || ui=0; }
settings="${XDG_CONFIG_HOME:-$HOME/.config}/wheelbri"
# Default --config: the mapping you saved in the app's Mapping tab for this target (configs/my-<name>.ini), else
# WheelBri's default one: the virtual G920 (uhid) or the in-game shim. The default was made on Windows; Linux numbers
# the rig's axes in another order, so on Linux save your own (Auto-map in the Mapping tab, then Save).
if [ -z "$config" ]; then
    [ "$mode" = shim ] && name=g920-in-game-shim.ini || name=g920-virtual-device.ini
    if [ -f "$settings/configs/my-$name" ]; then
        config="$settings/configs/my-$name"
    else
        config="$configs/$name"
    fi
fi

# Default --hide: every device of the active rig (app.ini [app] rig, then id = vvvv:pppp in rigs/<rig>.ini).
# The app writes its files with Windows line ends (CRLF), on Linux too: each line loses its \r first.
if [ -z "$hide" ]; then
    rig=$(awk -F= '{sub(/\r$/, "")} /^\[/{app = ($0 ~ /^\[app\]/); next} app && $1 ~ /^[ \t]*rig[ \t]*$/ {sub(/[;#].*/, "", $2); gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}' \
          "$settings/app.ini" 2>/dev/null)
    if [ -n "$rig" ] && [ -f "$settings/rigs/$rig.ini" ]; then
        hide=$(awk -F= '{sub(/\r$/, "")} $1 ~ /^[ \t]*id[ \t]*$/ {sub(/[;#].*/, "", $2); gsub(/[ \t]/, "", $2); split($2, p, ":");
                        if (length(p[1]) == 4 && length(p[2]) == 4 && toupper($2) != "046D:C262")
                            printf "%s0x%s/0x%s", (n++ ? "," : ""), toupper(p[1]), toupper(p[2])}' "$settings/rigs/$rig.ini")
    fi
    if [ -z "$hide" ] && [ -n "$rig" ] && [ -f "$settings/rigs/$rig.ini" ]; then
        echo "wheelbri.sh: rig '$rig' names no device id (vvvv:pppp): nothing is hidden from the game" >&2
    elif [ -z "$hide" ]; then
        echo "wheelbri.sh: no rig yet (set it up in the Rig tab of wheelbri-service --ui): nothing is hidden from the game" >&2
    fi
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

# --- the shim in the game's folder (--shim) ---
# The game's folder: --game-folder, else the folder of the last .exe in Steam's %command% (Proton's command ends with
# the game's own .exe; the ones before it are Steam's and Proton's programs).
find_game_folder() {
    if [ -n "$game_folder" ]; then
        printf '%s\n' "$game_folder"
        return
    fi
    local arg found=
    for arg in "${game[@]}"; do
        case "${arg,,}" in
            *.exe) [ -f "$arg" ] && found=$(dirname "$arg") ;;
        esac
    done
    printf '%s\n' "$found"
}

shim_dll=
for candidate in "${shim_dlls[@]}"; do
    if [ -f "$candidate" ]; then shim_dll=$candidate; break; fi
done
shim_folder=      # where this run put the shim; empty: nothing to take out
shim_put_dll=0    # this run put (or found its own) dinput8.dll there
shim_put_ini=0    # this run put wheelbri-shim.ini there

# Puts dinput8.dll and wheelbri-shim.ini into the game's folder. Never touches another mod's dinput8.dll: one that is
# neither ours (byte for byte) nor next to a wheelbri-shim.ini (an older WheelBri shim). A wheelbri-shim.ini already
# there (your own settings) is used as it is.
put_shim() {
    local folder dll ini
    folder=$(find_game_folder)
    if [ -z "$folder" ]; then
        say "--shim: no game folder (no .exe in the game's command): pass --game-folder DIR, or put dinput8.dll there yourself"
        [ ${#game[@]} -gt 0 ] && return 1
        return 0   # from a terminal without a game: the shim may already be in place
    fi
    [ -d "$folder" ] || { say "--shim: $folder is not a folder"; return 1; }
    [ -n "$shim_dll" ] || { say "--shim: dinput8.dll not found (${shim_dlls[*]}): build or install it first"; return 1; }
    dll="$folder/dinput8.dll"
    ini="$folder/wheelbri-shim.ini"
    if [ -e "$dll" ] && ! cmp -s "$shim_dll" "$dll" && [ ! -e "$ini" ]; then
        say "--shim: $dll belongs to another mod (not WheelBri's): left alone, nothing put in; remove it first"
        return 1
    fi
    if ! cmp -s "$shim_dll" "$dll" 2>/dev/null; then
        cp -f "$shim_dll" "$dll" || { say "--shim: cannot copy dinput8.dll into $folder"; return 1; }
    fi
    shim_folder=$folder
    shim_put_dll=1
    if [ ! -e "$ini" ]; then
        if cp "$configs/wheelbri-shim.ini" "$ini"; then
            shim_put_ini=1
        else
            say "--shim: cannot copy wheelbri-shim.ini into $folder: the shim uses its defaults"
        fi
    elif cmp -s "$configs/wheelbri-shim.ini" "$ini"; then
        shim_put_ini=1   # WheelBri's own settings, left from an earlier --keep-shim: ours to take out
    else
        say "--shim: using your own $ini (kept afterwards)"
    fi
    say "shim put into $folder"
    return 0
}

# Takes out what put_shim put in (unless --keep-shim): only files still identical to WheelBri's.
take_shim_out() {
    [ -n "$shim_folder" ] || return 0
    if [ "$keep_shim" = 1 ]; then
        say "shim left in $shim_folder (--keep-shim)"
        return 0
    fi
    local removed=
    if [ "$shim_put_dll" = 1 ] && cmp -s "$shim_dll" "$shim_folder/dinput8.dll"; then
        rm -f "$shim_folder/dinput8.dll" && removed="dinput8.dll"
    fi
    if [ "$shim_put_ini" = 1 ] && cmp -s "$configs/wheelbri-shim.ini" "$shim_folder/wheelbri-shim.ini"; then
        rm -f "$shim_folder/wheelbri-shim.ini" && removed="$removed${removed:+, }wheelbri-shim.ini"
    fi
    say "shim taken out of $shim_folder (${removed:-nothing to remove})"
    shim_folder=
}

# --virtual: a WheelBri shim left in the game's folder (--keep-shim, or put in by hand) is taken out, so the game
# finds only the virtual G920. Ours means: identical to this dinput8.dll, or next to a wheelbri-shim.ini (an older
# WheelBri shim). Another mod's dinput8.dll stays; a wheelbri-shim.ini with your own settings stays too.
remove_leftover_shim() {
    local folder removed=
    folder=$(find_game_folder)
    [ -n "$folder" ] && [ -d "$folder" ] || return 0
    local dll="$folder/dinput8.dll" ini="$folder/wheelbri-shim.ini"
    if [ -e "$dll" ] && { { [ -n "$shim_dll" ] && cmp -s "$shim_dll" "$dll"; } || [ -e "$ini" ]; }; then
        rm -f "$dll" && removed="dinput8.dll"
    fi
    if [ -e "$ini" ] && cmp -s "$configs/wheelbri-shim.ini" "$ini"; then
        rm -f "$ini" && removed="$removed${removed:+, }wheelbri-shim.ini"
    elif [ -e "$ini" ] && [ -n "$removed" ]; then
        say "--virtual: kept your own $ini (no effect without dinput8.dll)"
    fi
    [ -n "$removed" ] && say "--virtual: took the shim out of $folder ($removed)"
    return 0
}

# A session already running, the app's (wheelbri, Start) or wheelbri-service's: it holds this lock (SingleInstance).
lock="${XDG_RUNTIME_DIR:-/tmp}/wheelbri-service-$(id -u).lock"
session_running() {
    [ -e "$lock" ] && ! flock -n "$lock" true 2>/dev/null
}

start() {
    rm -f "$service_stop" "$uhid_stop"
    # The game's folder first: a refused shim (another mod's dinput8.dll) starts nothing.
    if [ "$mode" = shim ]; then
        put_shim || return 1
    else
        remove_leftover_shim
    fi
    if session_running || pgrep -f '(^|/)wheelbri-service( |$)' >/dev/null; then
        say "a WheelBri session is already running (the app or wheelbri-service): using it (and leaving it running afterwards)"
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
        say "hidden from the game: ${hide:-nothing}"
        export PROTON_ENABLE_HIDRAW="${PROTON_ENABLE_HIDRAW:+$PROTON_ENABLE_HIDRAW,}0x046D/0xC262"
        export SDL_GAMECONTROLLER_IGNORE_DEVICES="${SDL_GAMECONTROLLER_IGNORE_DEVICES:+$SDL_GAMECONTROLLER_IGNORE_DEVICES,}$hide"
        # Steam's virtual pad (28de:11ff) is never hidden: it is the only thing that controls the Steam Deck.
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
    trap '' INT TERM HUP
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
    take_shim_out
    rm -f "$service_stop" "$uhid_stop"
}
trap 'stop; exit 130' INT
trap 'stop; exit 143' TERM
trap 'stop; exit 129' HUP   # the terminal closed: stop cleanly rather than leave the service and the G920 behind

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
