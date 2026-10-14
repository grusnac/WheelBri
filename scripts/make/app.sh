#!/usr/bin/env bash
# Build everything (core, app, service, helper, shim, tests; configures the build tree the first time; on Windows also
# the 32-bit shim, into build-x86, when MSYS2's 32-bit compiler is installed) and start the
# app (wheelbri.exe: a window with Start, Stop and Open logs). --test runs the whole unit test suite between the two,
# and starts nothing when a test fails. A development build starts as Administrator (through PowerShell, which asks),
# so it does the privileged steps itself; --as-user starts it as you, as an installed WheelBri runs: then the WheelBri
# Helper must run (wheelbri-helper.exe --console from an Administrator shell, for this build folder).
#
#   scripts/make/app.sh                               build everything, start as Administrator
#   scripts/make/app.sh --test                        build everything, run all tests, then start
#   scripts/make/app.sh --as-user                     build and start as you (needs the helper)
#   scripts/make/app.sh --no-start                    only build (with --test: build and test)
#   scripts/make/app.sh --config my.ini               start with another config
#   WE_BUILD_TYPE=Debug scripts/make/app.sh           a debug build

source "$(dirname "$0")/common.sh"

start=1
as_user=0
tests=0
while [ $# -gt 0 ]; do
    case "$1" in
        --no-start) start=0; shift ;;
        --as-user) as_user=1; shift ;;
        --test) tests=1; shift ;;
        *) break ;;
    esac
done

cmake_build
build_shim_x86
if [ $tests -eq 1 ]; then
    say "running all unit tests..."
    ctest --test-dir "$build_dir" --output-on-failure || fail "tests failed: the app is not started"
    ok "tests passed"
fi
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
