#!/usr/bin/env bash
# Builds the Linux bundle dist/wheelbri-linux-x86_64.tar.gz (Steam Deck or any x86-64 distribution):
#   wheelbri/bin/       wheelbri (the app), wheelbri-service, wheelbri-uhid (C++ runtime linked in; they need only
#                       libX11 and the kernel: the wheel through evdev, the window through X11/XWayland)
#   wheelbri/config/    the example configs
#   wheelbri/shim/      dinput8.dll for --shim and the app's Games panel, and x86/dinput8.dll for 32-bit games,
#                       when given
#   wheelbri/           wheelbri.sh, install-udev-rules.sh, LINUX.md
#
# spdlog is always built in (a system one would be a shared library the Deck does not have).
# usage: scripts/linux/package.sh [--shim path/to/dinput8.dll] [--shim-x86 path/to/dinput8.dll] [-- cmake arguments]
#   --shim       the dinput8.dll built with MinGW (cmake/toolchain-mingw64.cmake)
#   --shim-x86   the 32-bit one (cmake/toolchain-mingw32.cmake), for 32-bit games

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
shim=
shim_x86=
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --shim) shift; shim=$1 ;;
        --shim-x86) shift; shim_x86=$1 ;;
        --) shift; extra=("$@"); break ;;
        *) sed -n '2,14p' "$0"; exit 2 ;;
    esac
    shift
done

build="$root/build-linux-release"
dist="$root/dist"
bundle="$dist/wheelbri"
rm -rf "$bundle"
mkdir -p "$bundle"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_TESTS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER \
    "${extra[@]}"
cmake --build "$build" -j "$(nproc)"
cmake --install "$build" --prefix "$bundle"

mkdir -p "$bundle/config"
cp "$root"/config/*.ini "$bundle/config/"
cp "$root/scripts/linux/wheelbri.sh" "$root/scripts/linux/install-udev-rules.sh" "$bundle/"
cp "$root/docs/linux.md" "$bundle/LINUX.md"
if [ -n "$shim" ]; then
    mkdir -p "$bundle/shim"
    cp "$shim" "$bundle/shim/dinput8.dll"   # in shim/, as in the Windows package: where the app looks for it
fi
if [ -n "$shim_x86" ]; then
    mkdir -p "$bundle/shim/x86"
    cp "$shim_x86" "$bundle/shim/x86/dinput8.dll"   # shim/x86/, as in the Windows package
fi
chmod +x "$bundle"/*.sh "$bundle"/bin/*

tar -czf "$dist/wheelbri-linux-x86_64.tar.gz" -C "$dist" wheelbri
echo "built $dist/wheelbri-linux-x86_64.tar.gz"
ls -l "$bundle" "$bundle/bin"
