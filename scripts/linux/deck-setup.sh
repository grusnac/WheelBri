#!/usr/bin/env bash
# Once, on the Steam Deck (Desktop Mode, Konsole), from the WheelBri checkout: the tools to build WheelBri there.
#
# SteamOS keeps its own system read-only and has no compiler, so the build runs in a distrobox container
# (distrobox and podman come with SteamOS 3.5 and later; nothing on SteamOS itself changes):
#   wheelbri-build   Debian 12 with GCC 12, CMake 3.25, Ninja, the X11 headers (the app's window), git, and MinGW-w64
#                    (the game shim, dinput8.dll).
# Debian 12's C library (glibc 2.36) is older than SteamOS's, so what it builds runs on SteamOS itself, outside the
# container. Your home folder is shared with it: the checkout and ~/wheelbri are the same files inside and out.
#
# Then, after every `git pull`: scripts/linux/deck-build.sh
#
# usage: scripts/linux/deck-setup.sh [--recreate]
#   --recreate   remove the container and make it again (a broken or outdated one; your files are not in it)

set -eu

container=wheelbri-build
image=docker.io/library/debian:12
packages=(build-essential cmake ninja-build git ca-certificates pkg-config libx11-dev
          g++-mingw-w64-x86-64-posix)

say()  { printf '\033[36m%s\033[0m\n' "$*"; }
ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }

recreate=0
case "${1:-}" in
    "") ;;
    --recreate) recreate=1 ;;
    *) sed -n '2,17p' "$0"; exit 2 ;;
esac

if [ -n "${CONTAINER_ID:-}" ] || [ -e /run/.containerenv ]; then
    fail "run this on SteamOS itself (Konsole), not inside a container"
fi
command -v distrobox >/dev/null 2>&1 ||
    fail "distrobox not found: it comes with SteamOS 3.5 and later (Settings > System > Software updates)"
command -v podman >/dev/null 2>&1 || fail "podman not found: it comes with distrobox on SteamOS 3.5 and later"

exists() { distrobox list --no-color 2>/dev/null | awk -F'|' 'NR > 1 { gsub(/ /, "", $2); print $2 }' | grep -qx "$container"; }

if [ "$recreate" = 1 ] && exists; then
    say "removing the container $container"
    distrobox rm --force "$container"
fi
if exists; then
    ok "container $container is there"
else
    say "making the container $container from $image (downloads ~50 MB the first time)"
    distrobox create --yes --name "$container" --image "$image"
fi

say "installing the build tools in $container (the first time: a few minutes, ~400 MB)"
# The first `enter` also sets the container up for distrobox; sudo needs no password inside it.
distrobox enter "$container" -- sudo env DEBIAN_FRONTEND=noninteractive apt-get update -q
distrobox enter "$container" -- sudo env DEBIAN_FRONTEND=noninteractive \
    apt-get install -y -q --no-install-recommends "${packages[@]}"

say "what the container has:"
# shellcheck disable=SC2016  # expanded in the container, not here
distrobox enter "$container" -- sh -c \
    'gcc --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"; git --version;
     x86_64-w64-mingw32-g++ --version | head -1; ldd --version | head -1'

ok "ready. Build and install WheelBri with: scripts/linux/deck-build.sh"
if [ ! -e /etc/udev/rules.d/70-wheelbri.rules ]; then
    say "once, after the first build: ~/wheelbri/install-udev-rules.sh (asks for your sudo password; set one with passwd)"
fi
