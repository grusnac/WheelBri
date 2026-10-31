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
#   scripts/make/g923-capture.sh setup-python       a portable Python (python.org's embeddable zip) in
#                                                   tools/g923-capture/python: nothing installed, delete the folder to
#                                                   remove it; its signature is checked (Python Software Foundation)
#
# Captures go to captures/g923/<date>/ (git-ignored); G923_DAY=<folder name> picks another day's folder.

source "$(dirname "$0")/common.sh"

kit="$root/tools/g923-capture"
day_name=${G923_DAY:-$(date +%F)}
day="$root/captures/g923/$day_name"

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ $# -ge 1 ] || usage
command=$1
shift

# A Windows Python: the kit's portable one first (setup-python), else one on PATH (MSYS2 UCRT64's is one; MSYS's own
# /usr/bin/python is not: the probe needs Windows' HID).
portable_version=3.12.10
portable="$kit/python"
find_python() {
    local p
    if [ -x "$portable/python.exe" ]; then
        echo "$portable/python.exe"
        return 0
    fi
    for p in python python3; do
        if command -v "$p" >/dev/null 2>&1 && [ "$("$p" -c 'import sys; print(sys.platform)' 2>/dev/null | tr -d '\r')" = win32 ]; then
            command -v "$p"
            return 0
        fi
    done
    return 1
}

python_unix=""
python_win=""
need_python() {
    python_unix=$(find_python) || fail "no Python for the kit: scripts/make/g923-capture.sh setup-python"
    python_win=$(cygpath -w "$python_unix")
}

run_ps() {
    on_windows || fail "the capture is Windows only (MSYS2 UCRT64 shell)"
    mkdir -p "$day"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$kit/g923-capture.ps1")" \
        -Repo "$(cygpath -w "$root")" -Day "$(cygpath -w "$day")" ${python_win:+-Python "$python_win"} "$@"
}

# name=exe folder lines from the PowerShell part (Steam's libraries), for acc, dirt and nfsheat.
game_folder() {
    run_ps -Step games | tr -d '\r' | sed -n "s/^$1=//p" | head -n 1
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
        [ $had_ini = 1 ] && cp -n "$ini" "$ini.before-g923"
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
    ok "shim in $key, logging only: $ini"
}

shim_restore() {
    [ -f "$shim_state" ] || { say "no shim was put in by the capture"; return 0; }
    local key folder had_dll had_ini ini
    while IFS='|' read -r key folder had_dll had_ini; do
        [ -n "$folder" ] || continue
        ini="$folder/wheelbri-shim.ini"
        if [ "$had_dll" = 0 ]; then
            bash "$root/scripts/make/shim.sh" --off "$(cygpath -w "$folder")" || warn "could not take the shim out of $key"
        fi
        if [ -f "$ini.before-g923" ]; then
            mv -f "$ini.before-g923" "$ini" && ok "$key: wheelbri-shim.ini back as it was"
        elif [ "$had_ini" = 0 ] && [ -f "$ini" ]; then
            mv -f "$ini" "$ini.g923" && ok "$key: the capture's wheelbri-shim.ini renamed to wheelbri-shim.ini.g923"
        fi
    done <"$shim_state"
    mv -f "$shim_state" "$shim_state.done"
}

print_summary() {
    local last
    last=$(tr -d '\r' <"$day/last-session.txt" 2>/dev/null) || return 0
    [ -n "$last" ] || return 0
    last=$(cygpath -u "$last")
    [ -f "$last/summary.md" ] && { say "---- $last/summary.md (top)"; sed -n '1,25p' "$last/summary.md"; }
}

case "$command" in
    check)
        need_python
        if [ "${1:-}" = "--wheelbri" ]; then run_ps -Step check -WheelBri; else run_ps -Step check; fi
        [ -f "$day/S0-check/check.md" ] && say "saved: $day/S0-check/check.md"
        ;;
    session)
        [ $# -eq 1 ] || fail "usage: g923-capture.sh session S1|S2|S3|S4|S5"
        s=$(echo "$1" | tr '[:lower:]' '[:upper:]')
        need_python
        case "$s" in
            S2|S3) shim_on acc ;;
            S4) shim_on dirt ;;
            S1|S5) ;;
            *) fail "no session $1 (S1..S5)" ;;
        esac
        run_ps -Step session -Session "$s"
        print_summary
        ;;
    probe)
        need_python
        on_windows || fail "the probe is Windows only"
        ghub=closed
        tasklist.exe 2>/dev/null | grep -qi '^lghub' && ghub=running
        say "probing the G923 (G HUB $ghub); read-only requests only"
        "$python_unix" -I "$kit/run.py" probe --out "$(cygpath -w "$day/S6-probe")" --repo "$(cygpath -w "$root")" --ghub "$ghub" \
            || fail "the probe failed (see above)"
        [ $ghub = running ] && say "now close G HUB fully (tray > Quit) and run probe once more"
        ;;
    decode)
        need_python
        target=${1:-$day}
        [ -d "$target" ] || fail "no folder $target"
        "$python_unix" -I "$kit/run.py" decode "$(cygpath -w "$target")" --repo "$(cygpath -w "$root")"
        status=$?
        [ -f "$target/report.md" ] && say "report: $target/report.md"
        exit $status
        ;;
    restore)
        shim_restore
        need_python
        run_ps -Step restore
        ;;
    setup-python)
        on_windows || fail "setup-python is for Windows (MSYS2 UCRT64 shell)"
        if [ -x "$portable/python.exe" ]; then
            ok "portable Python already there: $("$portable/python.exe" -c 'import sys; print(sys.version.split()[0], sys.platform)')"
            exit 0
        fi
        url="https://www.python.org/ftp/python/$portable_version/python-$portable_version-embed-amd64.zip"
        say "downloading $url"
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
            \$ErrorActionPreference = 'Stop'
            \$dest = '$(cygpath -w "$portable")'
            \$zip = Join-Path \$env:TEMP 'g923-python-embed.zip'
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri '$url' -OutFile \$zip
            New-Item -ItemType Directory -Force -Path \$dest | Out-Null
            Expand-Archive -LiteralPath \$zip -DestinationPath \$dest -Force
            Remove-Item -LiteralPath \$zip
            \$sig = Get-AuthenticodeSignature (Join-Path \$dest 'python.exe')
            if (\$sig.Status -ne 'Valid' -or \$sig.SignerCertificate.Subject -notmatch 'Python Software Foundation') {
                Remove-Item -LiteralPath \$dest -Recurse -Force
                Write-Host ('python.exe is not signed by the Python Software Foundation (' + \$sig.Status + '): removed') -ForegroundColor Red
                exit 1
            }
            Write-Host ('signature ok: ' + \$sig.SignerCertificate.Subject)
        " || fail "the portable Python was not set up"
        [ -x "$portable/python.exe" ] || fail "no python.exe after unpacking"
        ok "portable Python ready: $("$portable/python.exe" -c 'import sys, ctypes; print(sys.version.split()[0], sys.platform)' | tr -d '\r')"
        say "next: scripts/make/g923-capture.sh test"
        ;;
    test)
        py=$(find_python || command -v python3 || command -v python) || fail "no Python: scripts/make/g923-capture.sh setup-python"
        "$py" -m unittest discover -s "$kit/tests" -v
        ;;
    *) usage ;;
esac
