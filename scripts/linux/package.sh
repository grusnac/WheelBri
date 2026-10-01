#!/usr/bin/env bash
# Builds the Linux bundle dist/wheelemu-linux-x86_64.tar.gz (Steam Deck or any x86-64 distribution):
#   wheelemu/bin/       wheelemu-service, wheelemu-uhid (C++ runtime linked in)
#   wheelemu/lib/       libSDL3.so.0 (the service finds it there)
#   wheelemu/config/    the example configs
#   wheelemu/           wheelemu.sh, install-udev-rules.sh, LINUX.md, and dinput8.dll for --shim when given
#
# spdlog is always built in (a system one would be a shared library the Deck does not have).
# usage: scripts/linux/package.sh [--sdl3 PREFIX] [--shim path/to/dinput8.dll] [-- more cmake arguments]
#   --sdl3   where SDL3 is installed, if not system-wide (built with X11, Wayland and libudev for the --ui
#            window and hot-plugging)
#   --shim   the dinput8.dll built with MinGW (cmake --preset or cmake/toolchain-mingw64.cmake)

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
sdl3=
shim=
extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --sdl3) shift; sdl3=$1 ;;
        --shim) shift; shim=$1 ;;
        --) shift; extra=("$@"); break ;;
        *) sed -n '2,13p' "$0"; exit 2 ;;
    esac
    shift
done

build="$root/build-linux-release"
dist="$root/dist"
bundle="$dist/wheelemu"
rm -rf "$bundle"
mkdir -p "$bundle/lib"

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DWE_BUILD_TESTS=OFF \
    ${sdl3:+-DCMAKE_PREFIX_PATH="$sdl3"} \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib' \
    -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER \
    "${extra[@]}"
cmake --build "$build" -j "$(nproc)"
cmake --install "$build" --prefix "$bundle"

# SDL3 next to the programs (it loads X11/Wayland/udev itself when they are there).
sdl_lib=$(ldd "$build/service/wheelemu-service" | awk '/libSDL3\.so/ {print $3}')
[ -n "$sdl_lib" ] || { echo "libSDL3.so not found by ldd"; exit 1; }
cp -L "$sdl_lib" "$bundle/lib/libSDL3.so.0"

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
ls -l "$bundle" "$bundle/bin" "$bundle/lib"
