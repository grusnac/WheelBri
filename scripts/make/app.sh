#!/usr/bin/env bash
# Build the app (wheelbri.exe: a window with Start, Stop and Open logs) and start it. A development build starts as
# Administrator (through PowerShell, which asks), so it does the privileged steps itself; --as-user starts it as you,
# as an installed WheelBri runs: then the WheelBri Helper must run (wheelbri-helper.exe --console from an
# Administrator shell, for this build folder).
#
#   scripts/make/app.sh                               build and start as Administrator
#   scripts/make/app.sh --as-user                     build and start as you (needs the helper)
#   scripts/make/app.sh --no-start                    only build
#   scripts/make/app.sh --config my.ini               start with another config

source "$(dirname "$0")/common.sh"

start=1
as_user=0
while [ $# -gt 0 ]; do
    case "$1" in
        --no-start) start=0; shift ;;
        --as-user) as_user=1; shift ;;
        *) break ;;
    esac
done

cmake_build wheelbri
if on_windows; then
    app="$build_dir/service/wheelbri.exe"
else
    app="$build_dir/service/wheelbri"
fi
ok "built: $app"
[ $start -eq 1 ] || exit 0

if on_windows; then
    app_args=""
    if [ $# -gt 0 ]; then
        # Paths after --config become Windows paths; everything is passed as one argument string.
        for arg in "$@"; do
            [ -e "$arg" ] && arg=$(cygpath -w "$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")")
            app_args+="\"$arg\" "
        done
    fi
    verb="-Verb RunAs"
    if [ $as_user -eq 1 ]; then
        verb=""
        say "starting $app ${app_args}as you (the WheelBri Helper must be running)"
    else
        say "starting $app ${app_args}(answer the Administrator prompt)"
    fi
    if [ -n "$app_args" ]; then
        powershell.exe -NoProfile -Command "Start-Process $verb -FilePath $(ps_quote "$(cygpath -w "$root/$app")") -ArgumentList $(ps_quote "${app_args% }")" \
            || fail "the app did not start (Administrator refused?)"
    else
        powershell.exe -NoProfile -Command "Start-Process $verb -FilePath $(ps_quote "$(cygpath -w "$root/$app")")" \
            || fail "the app did not start (Administrator refused?)"
    fi
else
    "$app" "$@"
fi
