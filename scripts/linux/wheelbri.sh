#!/usr/bin/env bash
# Linux / Steam Deck: starts everything for a game with your rig as a G25, G27, G920 or G923, and stops it again.
#
# Two ways for the game to see the wheel:
#   --virtual (default)  wheelbri-uhid makes a virtual wheel for the whole system; the game reads it raw
#                        (PROTON_ENABLE_HIDRAW) and the rig is hidden from it (SDL_GAMECONTROLLER_IGNORE_DEVICES).
#                        A WheelBri shim left in the game's folder (--keep-shim) is taken out first.
#   --shim               the dinput8.dll proxy answers as the wheel inside the game: dinput8.dll and wheelbri-shim.ini
#                        are put into the game's folder (the one with the .exe in %command%) before it starts and
#                        taken out when it ends; WINEDLLOVERRIDES makes Proton load it. As the app does: the 32-bit
#                        dinput8.dll for a 32-bit game (shim/x86/), and dinput8Hooked.dll next to the Ultimate ASI
#                        Loader (a dinput8.dll mods use), which then loads it. Another mod's dinput8.dll is chained:
#                        renamed to dinput8-chain.dll, the shim hands DirectInput on to it; given back afterwards.
#
# As a Steam launch option (works in Game Mode; the game's own exit stops everything):
#   /home/deck/wheelbri/wheelbri.sh %command%
#   /home/deck/wheelbri/wheelbri.sh --shim %command%
# From a terminal (Desktop Mode), without a game: starts, waits for Enter or Ctrl+C, stops.
#   ./wheelbri.sh            (the service with its --ui window)
#
# Options: --virtual / --shim, --target g25|g27|g920|g923 (the game's wheel; default: the app's Mapping tab Target,
#          else the G920), --keep-shim (leave the shim in the game's folder afterwards, for testing),
#          --game-folder DIR (where --shim puts it; default: the folder of the .exe in %command%, else the Steam
#          game's install folder),
#          --ui / --no-ui (default: window only from a terminal), --config FILE (default: the mapping saved in
#          the app's Mapping tab, else WheelBri's default for the target),
#          --hide VID/PID[,VID/PID...] (the devices to hide from the game; default: every device of the active rig,
#          set up in the app's Rig tab: bin/wheelbri).
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
    shim_dlls_x86=("$here/shim/x86/dinput8.dll")
else
    root=$(cd "$here/../.." && pwd)
    build="$root/build"   # the Steam Deck's deck-build.sh builds in build-deck/
    [ -x "$build/service/wheelbri-service" ] || [ ! -x "$root/build-deck/service/wheelbri-service" ] || build="$root/build-deck"
    service="$build/service/wheelbri-service"
    uhid="$build/tools/uhid/wheelbri-uhid"
    configs="$root/config"
    shim_dlls=("$root/build-deck-shim/shim/dinput8.dll" "$root/build-win/shim/dinput8.dll")
    shim_dlls_x86=("$root/build-deck-shim-x86/shim/dinput8.dll" "$root/build-win-x86/shim/dinput8.dll")
fi

mode=uhid
target=   # empty: the app's Target (below)
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
        --target) shift; target=${1:-} ;;
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
# A key of app.ini's [app] (the app's choices), read as the app reads it: the last line wins, comments and spaces go.
# The app writes its files with Windows line ends (CRLF), on Linux too: each line loses its \r first.
app_setting() {
    awk -F= -v key="$1" '{sub(/\r$/, "")} /^\[/{app = ($0 ~ /^\[app\][ \t]*$/); next}
        app {k = $1; gsub(/^[ \t]+|[ \t]+$/, "", k)
             if (k == key) {v = substr($0, index($0, "=") + 1); sub(/[;#].*/, "", v); gsub(/^[ \t]+|[ \t]+$/, "", v)}}
        END {print v}' "$settings/app.ini" 2>/dev/null
}
# Default --target: the app's Mapping tab Target (app.ini [app] target), else the G920.
if [ -z "$target" ]; then
    target=$(app_setting target)
    [ -n "$target" ] || target=g920
fi
case "$target" in
    g920) wheel=G920; wheel_id=0x046D/0xC262 ;;
    g25) wheel=G25; wheel_id=0x046D/0xC299 ;;
    g27) wheel=G27; wheel_id=0x046D/0xC29B ;;
    g923) wheel=G923; wheel_id=0x046D/0xC26E ;;
    *) echo "wheelbri.sh: --target $target: expected g25, g27, g920 or g923" >&2; exit 2 ;;
esac
# Default --config: the mapping you saved in the app's Mapping tab for this target (configs/my-<target>.ini, both
# modes), else WheelBri's default one (<target>.ini). Before there was one for both modes (2026-10-15) each mode had
# its own (my-<target>-virtual-device.ini, my-<target>-in-game-shim.ini): the newer of those, until the app has made
# my-<target>.ini from it. The default was made on Windows; Linux numbers the rig's axes in another order, so on Linux
# save your own (map it in the Mapping tab, then Save).
if [ -z "$config" ]; then
    own="$settings/configs/my-$target.ini"
    if [ ! -f "$own" ]; then
        own=""
        for older in "$settings/configs/my-$target-virtual-device.ini" \
                     "$settings/configs/my-$target-in-game-shim.ini"; do
            if [ -f "$older" ] && { [ -z "$own" ] || [ "$older" -nt "$own" ]; }; then own=$older; fi
        done
    fi
    if [ -n "$own" ]; then
        config=$own
    else
        config="$configs/$target.ini"
    fi
fi

# Default --hide: every device of the active rig (app.ini [app] rig, then id = vvvv:pppp in rigs/<rig>.ini).
if [ -z "$hide" ]; then
    rig=$(app_setting rig)
    if [ -n "$rig" ] && [ -f "$settings/rigs/$rig.ini" ]; then
        hide=$(awk -F= '{sub(/\r$/, "")} $1 ~ /^[ \t]*id[ \t]*$/ {sub(/[;#].*/, "", $2); gsub(/[ \t]/, "", $2); split($2, p, ":");
                        virtual = toupper($2) == "046D:C262" || toupper($2) == "046D:C299" ||
                                  toupper($2) == "046D:C29B" || toupper($2) == "046D:C26E"   # the virtual wheels
                        if (length(p[1]) == 4 && length(p[2]) == 4 && !virtual)
                            printf "%s0x%s/0x%s", (n++ ? "," : ""), toupper(p[1]), toupper(p[2])}' "$settings/rigs/$rig.ini")
    fi
    if [ -z "$hide" ] && [ -n "$rig" ] && [ -f "$settings/rigs/$rig.ini" ]; then
        echo "wheelbri.sh: rig '$rig' names no device id (vvvv:pppp): nothing is hidden from the game" >&2
    elif [ -z "$hide" ]; then
        echo "wheelbri.sh: no rig yet (set it up in the Rig tab of bin/wheelbri): nothing is hidden from the game" >&2
    fi
fi

state="${XDG_STATE_HOME:-$HOME/.local/state}/wheelbri"
mkdir -p "$state"
# The shim's settings to put into the game's folder: WheelBri's, presenting the target (target_profile).
shim_ini="$configs/wheelbri-shim.ini"
if [ "$target" != g920 ] && [ -f "$configs/wheelbri-shim.ini" ]; then
    shim_ini="$state/wheelbri-shim-$target.ini"
    sed "s/^target_profile *=[^;]*/target_profile = $target           /" "$configs/wheelbri-shim.ini" >"$shim_ini"
    # A wheelbri-shim.ini without the line (edited by hand): the shim's own default would be the G920.
    grep -q "^target_profile *= *$target\b" "$shim_ini" ||
        printf '\n[shim]\ntarget_profile = %s\n' "$target" >>"$shim_ini"
fi
# The target_profile a shim's settings file says (the last line in [shim], as the shim reads it; g920 when none).
shim_target_of() {
    local said
    said=$(awk -F= '{sub(/\r$/, "")} /^\[/{shim = ($0 ~ /^\[shim\]/); next}
                    shim && $1 ~ /^[ \t]*target_profile[ \t]*$/ {sub(/[;#].*/, "", $2); gsub(/^[ \t]+|[ \t]+$/, "", $2); t = $2}
                    END {print t}' "$1" 2>/dev/null)
    echo "${said:-g920}"
}
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
# The Steam game's install folder from its app id (AppId=N in %command%, else Steam's SteamAppId): the "installdir" of
# appmanifest_<id>.acf in whichever Steam library holds it (libraryfolders.vdf lists them).
steam_install_folder() {
    local id='' arg steam lib manifest dir
    for arg in "${game[@]}"; do
        case "$arg" in AppId=[0-9]*) id=${arg#AppId=} ;; esac
    done
    [ -n "$id" ] || id=${SteamAppId:-}
    case "$id" in ''|*[!0-9]*) return 0 ;; esac
    for steam in "$HOME/.local/share/Steam" "$HOME/.steam/steam"; do
        [ -d "$steam/steamapps" ] || continue
        while IFS= read -r lib; do   # one library per line (their paths may hold spaces)
            manifest="$lib/steamapps/appmanifest_$id.acf"
            [ -f "$manifest" ] || continue
            dir=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$manifest" | head -1)
            [ -n "$dir" ] && [ -d "$lib/steamapps/common/$dir" ] && { printf '%s\n' "$lib/steamapps/common/$dir"; return 0; }
        done < <(printf '%s\n' "$steam"
                 sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$steam/steamapps/libraryfolders.vdf" 2>/dev/null)
    done
}

# The game's folder: --game-folder; else the folder of the last .exe in Steam's %command% (Proton's command ends with
# the game's own .exe; the ones before it are Steam's and Proton's programs); else, for a game Steam starts through a
# launcher (NFS 2015: "steam2ea://launchgame/..." through the EA app, no .exe at all), the Steam install folder.
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
    [ -n "$found" ] || found=$(steam_install_folder)
    printf '%s\n' "$found"
}

# The first of these files that is there (nothing when none is).
first_file() {
    local candidate
    for candidate in "$@"; do
        if [ -f "$candidate" ]; then printf '%s\n' "$candidate"; return; fi
    done
}
shim_dll_x64=$(first_file "${shim_dlls[@]}")
shim_dll_x86=$(first_file "${shim_dlls_x86[@]}")

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
        machine=$(pe_machine "$exe")
        case "$machine" in
            014c) bits=32; largest=$size ;;
            8664) bits=64; largest=$size ;;
        esac
    done
    echo "$bits"
}

# The name the shim takes in the folder: dinput8.dll, or dinput8Hooked.dll when the folder's dinput8.dll is the
# Ultimate ASI Loader (it loads dinput8Hooked.dll in turn; its own file is never touched).
shim_name_in() {
    if [ -f "$1/dinput8.dll" ] && grep -qa IsUltimateASILoader "$1/dinput8.dll" 2>/dev/null &&
        ! grep -qa IsWheelBriShim "$1/dinput8.dll" 2>/dev/null; then   # the shim has the name too
        echo dinput8Hooked.dll
    else
        echo dinput8.dll
    fi
}

chain_name=dinput8-chain.dll   # another mod's dll, chained (core ShimGames.hpp: kShimChainDll)
chain_line="chain = $chain_name"

# Whether the settings file $1 sets [shim] chain.
ini_has_chain() {
    [ -f "$1" ] && awk -F= '{sub(/\r$/, "")} /^\[/{shim = ($0 ~ /^\[shim\]/); next}
                          shim && $1 ~ /^[ \t]*chain[ \t]*$/ {found = 1} END {exit !found}' "$1" 2>/dev/null
}

# Whether $1 (a dll in game folder $2) is a WheelBri shim: one of ours byte for byte, our mark (the IsWheelBriShim
# export), or an older shim without the mark: next to a wheelbri-shim.ini without chain (a shim that chains has the
# mark, so a dll without it there is a mod's update over our shim).
is_our_shim() {
    { [ -n "$shim_dll_x64" ] && cmp -s "$shim_dll_x64" "$1"; } ||
        { [ -n "$shim_dll_x86" ] && cmp -s "$shim_dll_x86" "$1"; } ||
        grep -qa IsWheelBriShim "$1" 2>/dev/null ||
        { [ -e "$2/wheelbri-shim.ini" ] && ! ini_has_chain "$2/wheelbri-shim.ini"; }
}

# Adds chain = dinput8-chain.dll to [shim] in the settings file $1, unless it sets chain already.
add_chain_line() {
    ini_has_chain "$1" && return 0
    if grep -q '^\[shim\]' "$1" 2>/dev/null; then
        sed -i "0,/^\[shim\]/s//[shim]\n$chain_line/" "$1"
    else
        printf '[shim]\n%s\n' "$chain_line" >>"$1"
    fi
}

# Whether the settings file $1 is WheelBri's own ($2), give or take the chain line put_shim added.
same_settings() {
    cmp -s "$2" "$1" || cmp -s "$2" <(grep -v -x -F "$chain_line" "$1")
}

shim_dll=         # WheelBri's dinput8.dll this run put in (the 64-bit or the 32-bit one)
shim_folder=      # where this run put the shim; empty: nothing to take out
shim_target=      # the file it went to there (dinput8.dll or dinput8Hooked.dll)
shim_put_dll=0    # this run put (or found its own) shim there
shim_put_ini=0    # this run put wheelbri-shim.ini there
shim_chained=0    # this run chained another mod's dll (renamed to dinput8-chain.dll)

# Puts the shim and wheelbri-shim.ini into the game's folder, as the app's Games tab does. Another mod's dll in its
# place (not is_our_shim) is chained: renamed to dinput8-chain.dll (one there from before becomes .old), and the
# settings get chain = dinput8-chain.dll; one built for the other processor than the game is left alone, nothing put in.
# A wheelbri-shim.ini already there (your own settings) is used as it is (the chain line added when chaining).
put_shim() {
    local folder dll ini bits name
    folder=$(find_game_folder)
    if [ -z "$folder" ]; then
        say "--shim: no game folder (no .exe and no Steam app id in the game's command): pass --game-folder DIR, or put dinput8.dll there yourself"
        [ ${#game[@]} -gt 0 ] && return 1
        return 0   # from a terminal without a game: the shim may already be in place
    fi
    [ -d "$folder" ] || { say "--shim: $folder is not a folder"; return 1; }
    bits=$(game_bits "$folder")
    if [ "$bits" = 32 ]; then
        shim_dll=$shim_dll_x86
        [ -n "$shim_dll" ] || {
            say "--shim: the game is 32-bit; no 32-bit dinput8.dll (${shim_dlls_x86[*]}): build or install it first"
            return 1
        }
    else
        shim_dll=$shim_dll_x64
        [ -n "$shim_dll" ] || { say "--shim: no dinput8.dll (${shim_dlls[*]}): build or install it first"; return 1; }
    fi
    name=$(shim_name_in "$folder")
    dll="$folder/$name"
    ini="$folder/wheelbri-shim.ini"
    if [ -e "$dll" ] && ! is_our_shim "$dll" "$folder"; then
        local theirs
        case "$(pe_machine "$dll")" in
            014c) theirs=32 ;;
            8664) theirs=64 ;;
            *) theirs=$bits ;;
        esac
        if [ "$theirs" != "$bits" ]; then
            say "--shim: $dll is another mod's, built for the other processor than the game: left alone, nothing put in"
            return 1
        fi
        if [ -e "$folder/$chain_name" ] && [ ! -f "$folder/$chain_name" ]; then
            say "--shim: $folder/$chain_name is not a file: another mod's $name cannot be chained, nothing put in"
            return 1
        fi
        if [ -f "$folder/$chain_name" ]; then   # the mod was updated over our shim: the one chained before kept
            mv -f "$folder/$chain_name" "$folder/$chain_name.old" || { say "--shim: cannot rename $chain_name"; return 1; }
        fi
        mv "$dll" "$folder/$chain_name" || { say "--shim: cannot rename $name to $chain_name"; return 1; }
        shim_chained=1
        say "--shim: another mod's $name chained: renamed to $chain_name, the shim hands DirectInput on to it"
    fi
    if ! cmp -s "$shim_dll" "$dll" 2>/dev/null; then
        if ! cp -f "$shim_dll" "$dll"; then
            say "--shim: cannot copy $name into $folder"
            [ "$shim_chained" = 1 ] && mv "$folder/$chain_name" "$dll"   # the mod gets its place back
            return 1
        fi
    fi
    shim_folder=$folder
    shim_target=$dll
    shim_put_dll=1
    if [ ! -e "$ini" ]; then
        if cp "$shim_ini" "$ini"; then
            shim_put_ini=1
        else
            say "--shim: cannot copy wheelbri-shim.ini into $folder: the shim uses its defaults (a G920)"
        fi
    elif same_settings "$ini" "$shim_ini"; then
        shim_put_ini=1   # WheelBri's own settings, left from an earlier --keep-shim: ours to take out
    else
        say "--shim: using your own $ini (kept afterwards)"
        local own
        own=$(shim_target_of "$ini")
        [ "$own" = "$target" ] ||
            say "--shim: WARNING: your $ini presents the $own, not the $target: set target_profile = $target there"
    fi
    if [ -f "$folder/$chain_name" ] && [ -e "$ini" ]; then   # the shim follows the chain only when told to
        add_chain_line "$ini" || say "--shim: cannot add '$chain_line' to $ini: the mod is not used"
    fi
    say "shim put into $folder ($bits-bit, as $name)"
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
    if [ "$shim_put_dll" = 1 ] && cmp -s "$shim_dll" "$shim_target"; then
        rm -f "$shim_target" && removed=$(basename "$shim_target")
    fi
    if [ -n "$removed" ] && [ -f "$shim_folder/$chain_name" ]; then   # the chained mod gets its place back
        mv "$shim_folder/$chain_name" "$shim_target" && removed="$removed, $chain_name given back"
    fi
    if [ "$shim_put_ini" = 1 ] && same_settings "$shim_folder/wheelbri-shim.ini" "$shim_ini"; then
        rm -f "$shim_folder/wheelbri-shim.ini" && removed="$removed${removed:+, }wheelbri-shim.ini"
    fi
    say "shim taken out of $shim_folder (${removed:-nothing to remove})"
    shim_folder=
}

# --virtual: a WheelBri shim left in the game's folder (--keep-shim, or put in by hand) is taken out, so the game
# finds only the virtual wheel. Ours means is_our_shim. Another mod's dll stays (the ASI Loader too), a chained one
# gets its place back; a wheelbri-shim.ini with your own settings stays too.
remove_leftover_shim() {
    local folder removed='' dll ini
    folder=$(find_game_folder)
    [ -n "$folder" ] && [ -d "$folder" ] || return 0
    dll="$folder/$(shim_name_in "$folder")"
    ini="$folder/wheelbri-shim.ini"
    if [ -e "$dll" ] && is_our_shim "$dll" "$folder"; then
        rm -f "$dll" && removed=$(basename "$dll")
    fi
    if [ -n "$removed" ] && [ -f "$folder/$chain_name" ]; then
        mv "$folder/$chain_name" "$dll" && removed="$removed, $chain_name given back"
    fi
    if [ -e "$ini" ] && { same_settings "$ini" "$configs/wheelbri-shim.ini" || same_settings "$ini" "$shim_ini"; }; then
        rm -f "$ini" && removed="$removed${removed:+, }wheelbri-shim.ini"
    elif [ -e "$ini" ] && [ -n "$removed" ]; then
        say "--virtual: kept your own $ini (no effect without the shim)"
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
    # The game's folder first: a refused shim (another mod's dinput8.dll that cannot be chained) starts nothing.
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
            "${clean_env[@]}" "$uhid" --stop-file "$uhid_stop" --target "$target" \
                >>"$state/wheelbri-uhid.console" 2>&1 &
            uhid_pid=$!
            sleep 1
            if ! kill -0 "$uhid_pid" 2>/dev/null; then
                say "wheelbri-uhid stopped at once: see $state/wheelbri-uhid-*.log (install-udev-rules.sh run?)"
                return 1
            fi
            say "virtual $wheel created (wheelbri-uhid)"
        fi
        # Raw HID for the virtual wheel (its force feedback is HID PID), and the real wheel hidden from the game.
        say "hidden from the game: ${hide:-nothing}"
        export PROTON_ENABLE_HIDRAW="${PROTON_ENABLE_HIDRAW:+$PROTON_ENABLE_HIDRAW,}$wheel_id"
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
        say "virtual $wheel removed"
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
trap 'stop; exit 129' HUP   # the terminal closed: stop cleanly rather than leave the service and the virtual wheel behind

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

say "running ($mode). Start the game$([ "$mode" = uhid ] && echo " with the launch option: PROTON_ENABLE_HIDRAW=$wheel_id SDL_GAMECONTROLLER_IGNORE_DEVICES=$hide %command%" || echo " with: WINEDLLOVERRIDES=\"dinput8=n,b\" %command%")"
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
