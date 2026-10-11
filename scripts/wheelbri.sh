#!/usr/bin/env bash
# Starts everything for NFS with the Moza R3 as a G920, from an MSYS2 UCRT64 shell, and stops everything
# again: wheelbri-service (with its window), then wheelbri-bridge (virtual G920 + Xbox controller) in its own
# Administrator window. Press Enter here (or Ctrl+C) to stop both cleanly, best after closing NFS.
#
#   scripts/wheelbri.sh                      start, wait for Enter, stop
#   scripts/wheelbri.sh --build              build the service and the bridge first
#   scripts/wheelbri.sh --no-ui              service without its window
#   scripts/wheelbri.sh --xbox-pad idle      Xbox controller mode: buttons (default), idle or off
#   scripts/wheelbri.sh --config FILE        another service config (default config/moza-r3-as-g920.ini)
#
# Windows asks for Administrator once per start (the bridge needs it). Logs: %LOCALAPPDATA%\wheelbri\logs.

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1

config=config/moza-r3-as-g920.ini
ui=1
pad=buttons
build=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=1 ;;
        --no-ui) ui=0 ;;
        --xbox-pad) shift; pad=${1:-}
            case "$pad" in buttons|idle|off) ;; *) echo "--xbox-pad: buttons, idle or off"; exit 2 ;; esac ;;
        --config) shift; config=${1:-} ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)"; exit 2 ;;
    esac
    shift
done

service=build/service/wheelbri-service.exe
bridge=tools/vbridge/bin/Release/net10.0-windows10.0.26100.0/wheelbri-bridge.exe
state="$(cygpath -u "${LOCALAPPDATA:-$USERPROFILE/AppData/Local}")/wheelbri"
service_stop="$state/service.stop"
bridge_stop="$state/bridge.stop"
mkdir -p "$state"

say()  { printf '\033[36m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }
win()  { cygpath -w "$1"; }

# Windows processes, also the bridge's (Administrator) one.
image_running() { tasklist //FI "IMAGENAME eq $1" //NH 2>/dev/null | grep -qi "$1"; }
pid_running()   { tasklist //FI "PID eq $1" //NH 2>/dev/null | grep -qi "\.exe"; }

image_running NFS16.exe && fail "NFS is running: close it first, the game must start after the bridge."
image_running wheelbri-bridge.exe && fail "wheelbri-bridge is already running: stop it first (Ctrl+C in its window)."
image_running wheelbri-service.exe && fail "wheelbri-service is already running: stop it first (close its window)."

if [ $build -eq 1 ]; then
    say "building the service..."
    cmake --build build || fail "service build failed"
    dotnet=$(command -v dotnet || echo "/c/Program Files/dotnet/dotnet.exe")
    say "building the bridge..."
    "$dotnet" build tools/vbridge/wheelbri-bridge.csproj -c Release -v q -nologo || fail "bridge build failed"
fi
[ -f "$service" ] || fail "missing $service: build it (cmake --build build) or use --build"
[ -f "$bridge" ] || fail "missing $bridge: build it (dotnet build tools/vbridge/wheelbri-bridge.csproj -c Release) or use --build"

rm -f "$service_stop" "$bridge_stop"

# 1. The service (it reads the R3; the bridge hides the R3 from games only while the service runs).
service_args=(--config "$config" --stop-file "$(win "$service_stop")")
[ $ui -eq 1 ] && service_args+=(--ui)
"$service" "${service_args[@]}" &
service_pid=$!
sleep 2
kill -0 "$service_pid" 2>/dev/null || fail "wheelbri-service stopped at once; see %LOCALAPPDATA%\\wheelbri\\logs"
say "wheelbri-service started ($config)"

# 2. The bridge, as Administrator, in its own window.
bridge_args="--stop-file \"$(win "$bridge_stop")\""
[ "$pad" != off ] && bridge_args="--xbox-pad $pad $bridge_args"
bridge_pid=$(powershell.exe -NoProfile -Command \
    "try { (Start-Process -FilePath '$(win "$root/$bridge")' -ArgumentList '$bridge_args' -Verb RunAs -PassThru -ErrorAction Stop).Id } catch { exit 1 }" \
    | tr -d '\r')
if [ -z "$bridge_pid" ]; then
    warn "the bridge did not start (Administrator refused?); stopping the service"
    touch "$service_stop"
    wait "$service_pid"
    exit 1
fi
say "wheelbri-bridge started as Administrator (its own window; Xbox controller: $pad)"

# 3. Wait: Enter or Ctrl+C here, or one of them stopping by itself.
stop_asked=0
trap 'stop_asked=1' INT
echo
say "Running. Start Steam and NFS. When done: close NFS, then press Enter here to stop everything."
reason="stop asked"
while :; do
    read -r -t 2
    status=$?
    [ $status -eq 0 ] && break                      # Enter
    [ $status -le 128 ] && sleep 2                  # no keyboard (input closed): just keep watching
    [ $stop_asked -eq 1 ] && break
    if ! pid_running "$bridge_pid"; then reason="the bridge stopped"; break; fi
    if ! kill -0 "$service_pid" 2>/dev/null; then reason="the service stopped"; break; fi
done
trap '' INT   # the stop below must not be cut short

# 4. Stop: the bridge first (un-hides the R3, removes the virtual devices), then the service.
echo
say "stopping ($reason)..."
image_running NFS16.exe && warn "NFS is still running: close it, the bridge waits until it lets go of the G920."
if pid_running "$bridge_pid"; then
    touch "$bridge_stop"
    waited=0
    while pid_running "$bridge_pid"; do
        sleep 1
        waited=$((waited + 1))
        [ $waited -eq 10 ] && warn "still waiting for the bridge: its window says which program holds the G920 (close it; Ctrl+C twice in that window removes the G920 anyway)."
    done
fi
say "wheelbri-bridge stopped"
if kill -0 "$service_pid" 2>/dev/null; then
    touch "$service_stop"
    for _ in $(seq 1 20); do
        kill -0 "$service_pid" 2>/dev/null || break
        sleep 0.5
    done
    kill -0 "$service_pid" 2>/dev/null && warn "wheelbri-service did not stop within 10 s: close its window."
    wait "$service_pid" 2>/dev/null
fi
say "wheelbri-service stopped"
rm -f "$service_stop" "$bridge_stop"
