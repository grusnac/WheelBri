#!/usr/bin/env bash
# On the Steam Deck (or any x86-64 Linux with distrobox), after every `git pull`: builds WheelBri in the
# wheelbri-build container (scripts/linux/deck-setup.sh makes it, once), runs the unit tests, and installs it into
# ~/wheelbri, where wheelbri.sh, the docs (LINUX.md) and Steam's launch option expect it:
#   ~/wheelbri/bin/       wheelbri (the app), wheelbri-service, wheelbri-uhid
#   ~/wheelbri/config/    the example configs; one you edited is kept, the new one goes next to it as <name>.new
#   ~/wheelbri/           wheelbri.sh, install-udev-rules.sh, LINUX.md, dinput8.dll (the shim, for --shim)
# The builds stay in the checkout (build-deck/, build-deck-shim/), so the next one only rebuilds what changed.
# Then it checks on SteamOS itself (outside the container) that every program finds its libraries.
#
# usage: scripts/linux/deck-build.sh [--no-tests] [--no-shim] [--clean] [--prefix DIR]
#   --no-tests   skip the unit tests (faster)
#   --no-shim    skip dinput8.dll (MinGW)
#   --clean      build from scratch (delete build-deck/ and build-deck-shim/ first)
#   --prefix     install somewhere else than ~/wheelbri

set -eu

container=wheelbri-build
root=$(cd "$(dirname "$0")/../.." && pwd)
build="$root/build-deck"
shim_build="$root/build-deck-shim"

say()  { printf '\033[36m%s\033[0m\n' "$*"; }
ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }

inside=0
tests=1
shim=1
clean=0
prefix="$HOME/wheelbri"
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --inside) inside=1 ;;
        --no-tests) tests=0; args+=("$1") ;;
        --no-shim) shim=0; args+=("$1") ;;
        --clean) clean=1; args+=("$1") ;;
        --prefix) shift; [ $# -gt 0 ] || fail "--prefix needs a folder"; prefix=$1; args+=(--prefix "$1") ;;
        *) sed -n '2,17p' "$0"; exit 2 ;;
    esac
    shift
done

# --- in the container: build, test, install ---------------------------------------------------------------------
if [ "$inside" = 1 ]; then
    cd "$root"
    if [ "$clean" = 1 ]; then
        say "building from scratch"
        rm -rf "$build" "$shim_build"
    fi
    jobs=$(nproc)
    test_switch=OFF
    if [ "$tests" = 1 ]; then test_switch=ON; fi

    say "configuring (the first time CMake downloads spdlog and GoogleTest)"
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DWE_BUILD_TESTS="$test_switch" \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER >/dev/null
    say "building with $jobs jobs"
    cmake --build "$build" -j "$jobs"
    if [ "$tests" = 1 ]; then
        say "unit tests"
        ctest --test-dir "$build" --output-on-failure   # one at a time: some tests use fixed UDP ports
    fi

    if [ "$shim" = 1 ]; then
        say "the game shim (dinput8.dll, MinGW)"
        cmake -S "$root" -B "$shim_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchain-mingw64.cmake" \
            -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER >/dev/null
        cmake --build "$shim_build" -j "$jobs"
    fi

    say "installing into $prefix"
    stage="$build/stage"
    rm -rf "$stage"
    cmake --install "$build" --prefix "$stage" >/dev/null
    mkdir -p "$prefix/bin" "$prefix/config"
    for program in "$stage"/bin/*; do
        # Replaced, not written over: a copy of the old program that is still open keeps working.
        cp -f "$program" "$prefix/bin/.$(basename "$program").new"
        mv -f "$prefix/bin/.$(basename "$program").new" "$prefix/bin/$(basename "$program")"
    done
    chmod +x "$prefix"/bin/*
    for config in "$root"/config/*.ini; do
        name=$(basename "$config")
        if [ ! -e "$prefix/config/$name" ]; then
            cp "$config" "$prefix/config/$name"
        elif ! cmp -s "$config" "$prefix/config/$name"; then
            cp "$config" "$prefix/config/$name.new"
            warn "kept your config/$name; this version's is config/$name.new"
        fi
    done
    cp -f "$root/scripts/linux/wheelbri.sh" "$root/scripts/linux/install-udev-rules.sh" "$prefix/"
    chmod +x "$prefix/wheelbri.sh" "$prefix/install-udev-rules.sh"
    cp -f "$root/docs/linux.md" "$prefix/LINUX.md"
    if [ "$shim" = 1 ]; then
        cp -f "$shim_build/shim/dinput8.dll" "$prefix/dinput8.dll"
    fi
    exit 0
fi

# --- on SteamOS: into the container, then check the result here ------------------------------------------------
if [ -n "${CONTAINER_ID:-}" ] || [ -e /run/.containerenv ]; then
    fail "run this on SteamOS itself (Konsole); it goes into the $container container by itself"
fi
command -v distrobox >/dev/null 2>&1 || fail "distrobox not found: run scripts/linux/deck-setup.sh first"
if ! distrobox list --no-color 2>/dev/null | awk -F'|' 'NR > 1 { gsub(/ /, "", $2); print $2 }' |
        grep -qx "$container"; then
    fail "no $container container yet: run scripts/linux/deck-setup.sh first"
fi
for program in wheelbri wheelbri-service wheelbri-uhid; do
    if pgrep -x "$program" >/dev/null 2>&1; then
        fail "$program is running: stop WheelBri (and the game) first, then build again"
    fi
done

started=$(date +%s)
distrobox enter "$container" -- "$root/scripts/linux/deck-build.sh" --inside ${args[@]+"${args[@]}"}

say "checking the programs on SteamOS itself"
for program in "$prefix"/bin/*; do
    # The loader lists what a program needs without running it, and says when a library or version is missing.
    if ! report=$(LD_TRACE_LOADED_OBJECTS=1 "$program" 2>&1) || grep -Eq 'not found' <<<"$report"; then
        printf '%s\n' "$report"
        fail "$(basename "$program") cannot run on this system (above: what is missing)"
    fi
done

ok "WheelBri $(sed -n 's/^    VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1) ($(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo '?')) installed in $prefix in $(( $(date +%s) - started )) s"
if [ ! -e /etc/udev/rules.d/70-wheelbri.rules ]; then
    warn "once: $prefix/install-udev-rules.sh (asks for your sudo password; set one with passwd)"
fi
echo "the app:            $prefix/bin/wheelbri"
echo "NFS launch option:  $prefix/wheelbri.sh %command%"
