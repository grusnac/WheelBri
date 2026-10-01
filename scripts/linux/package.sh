#!/usr/bin/env bash
# Builds the Linux bundle dist/wheelemu-linux-x86_64.tar.gz (Steam Deck or any x86-64 distribution):
#   wheelemu/bin/       wheelemu-service, wheelemu-uhid (C++ runtime linked in; they need only libX11 and
#                       the kernel: the wheel through evdev, the --ui window through X11/XWayland)
#   wheelemu/config/    the example configs
#   wheelemu/           wheelemu.sh, install-udev-rules.sh, LINUX.md, and dinput8.dll for --shim when given
#
# spdlog is always built in (a system one would be a shared library the Deck does not have).
# usage: scripts/linux/package.sh [--shim path/to/dinput8.dll] [-- more cmake arguments]
#   --shim   the dinput8.dll built with MinGW (cmake --preset or cmake/toolchain-mingw64.cmake)

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
shim=
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --shim) shift; shim=$1 ;;
        --) shift; extra=("$@"); break ;;
        *) sed -n '2,11p' "$0"; exit 2 ;;
    esac
    shift
done

build="$root/build-linux-release"
dist="$root/dist"
bundle="$dist/wheelemu"
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
cp "$root/scripts/linux/wheelemu.sh" "$root/scripts/linux/install-udev-rules.sh" "$bundle/"
cp "$root/docs/linux.md" "$bundle/LINUX.md"
if [ -n "$shim" ]; then
    cp "$shim" "$bundle/dinput8.dll"
fi
chmod +x "$bundle"/*.sh "$bundle"/bin/*

tar -czf "$dist/wheelemu-linux-x86_64.tar.gz" -C "$dist" wheelemu
echo "built $dist/wheelemu-linux-x86_64.tar.gz"
ls -l "$bundle" "$bundle/bin"
