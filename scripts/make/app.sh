#!/usr/bin/env bash
# Build the app (wheelemu.exe: a window with Start, Stop and Open logs) and start it. Windows asks for
# Administrator (the app's manifest requires it), so it is started through PowerShell rather than from bash.
#
#   scripts/make/app.sh                               build and start
#   scripts/make/app.sh --no-start                    only build
#   scripts/make/app.sh --config config/moza-r3-as-g29.ini   start with another config

source "$(dirname "$0")/common.sh"

start=1
if [ "${1:-}" = "--no-start" ]; then
    start=0
    shift
fi

cmake_build wheelemu
if on_windows; then
    app="$build_dir/service/wheelemu.exe"
else
    app="$build_dir/service/wheelemu"
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
    say "starting $app ${app_args}(answer the Administrator prompt)"
    if [ -n "$app_args" ]; then
        powershell.exe -NoProfile -Command "Start-Process -FilePath '$(cygpath -w "$root/$app")' -ArgumentList '${app_args% }'" \
            || fail "the app did not start (Administrator refused?)"
    else
        powershell.exe -NoProfile -Command "Start-Process -FilePath '$(cygpath -w "$root/$app")'" \
            || fail "the app did not start (Administrator refused?)"
    fi
else
    "$app" "$@"
fi
