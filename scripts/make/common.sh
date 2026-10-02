# shellcheck shell=bash
# Shared by the scripts in scripts/make (sourced, not run on its own).
#
# Environment overrides:
#   WE_BUILD_DIR    build tree (default: build)
#   WE_BUILD_TYPE   Release (default) or Debug
#   WE_CMAKE_ARGS   extra arguments for the CMake configure step

set -u

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root" || exit 1

build_dir=${WE_BUILD_DIR:-build}
build_type=${WE_BUILD_TYPE:-Release}

say()  { printf '\033[36m%s\033[0m\n' "$*"; }
ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }

on_windows() {
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; esac
    return 1
}

need() { command -v "$1" >/dev/null 2>&1 || fail "$1 not found: $2"; }

# Configure the CMake tree (always; cmake keeps what it can).
configure() {
    need cmake "MSYS2: pacman -S mingw-w64-ucrt-x86_64-cmake"
    need ninja "MSYS2: pacman -S mingw-w64-ucrt-x86_64-ninja"
    say "configuring $build_dir ($build_type)..."
    # shellcheck disable=SC2086  # WE_CMAKE_ARGS is a list of arguments
    cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" ${WE_CMAKE_ARGS:-} \
        || fail "configure failed"
}

# Configure only if there is no build tree yet, or it was made for another build type.
ensure_configured() {
    local cache="$build_dir/CMakeCache.txt"
    if [ ! -f "$cache" ]; then
        configure
    elif ! grep -q "^CMAKE_BUILD_TYPE:STRING=$build_type\$" "$cache"; then
        warn "$build_dir was configured for another build type; reconfiguring for $build_type"
        configure
    fi
}

# Incremental CMake build; arguments are targets (none = everything).
cmake_build() {
    ensure_configured
    local args=()
    [ $# -gt 0 ] && args=(--target "$@")
    say "building ${*:-everything} in $build_dir..."
    cmake --build "$build_dir" "${args[@]}" || fail "build failed"
}

service_exe() {
    if on_windows; then echo "$build_dir/service/wheelemu-service.exe"; else echo "$build_dir/service/wheelemu-service"; fi
}

dotnet_exe() {
    command -v dotnet 2>/dev/null && return
    [ -x "/c/Program Files/dotnet/dotnet.exe" ] && { echo "/c/Program Files/dotnet/dotnet.exe"; return; }
    return 1
}
