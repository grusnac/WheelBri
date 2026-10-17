#!/usr/bin/env bash
# Once, on the Steam Deck (Desktop Mode, Konsole), from the WheelBri checkout: the tools to build WheelBri there.
#
# SteamOS is Arch Linux underneath, but keeps its own system read-only and has no compiler. So the build runs in an
# Arch Linux distrobox container (distrobox and podman come with SteamOS 3.5 and later; nothing on SteamOS itself
# changes):
#   wheelbri-build   Arch Linux with base-devel (GCC), CMake, Ninja, git, libx11 (the app's window) and mingw-w64-gcc
#                    (the game shim, dinput8.dll: 64-bit and 32-bit, both in that package), from pacman.
# The container is pinned to the day Arch still shipped SteamOS's own glibc version (the Arch Linux Archive), so the
# programs it builds load on SteamOS itself, outside the container: today's Arch is usually ahead of SteamOS.
# Your home folder is shared with it: the checkout and ~/wheelbri are the same files inside and out.
#
# Then, after every `git pull`: scripts/linux/deck-build.sh
# After a SteamOS update that brings a newer glibc, deck-build.sh says so: run this again with --recreate.
#
# usage: scripts/linux/deck-setup.sh [--recreate]
#   --recreate   remove the container and make it again (outdated or broken; your files are not in it)

set -eu

container=wheelbri-build
image=docker.io/library/archlinux:latest
archive=https://archive.archlinux.org
packages=(base-devel cmake ninja git libx11 mingw-w64-gcc)

say()  { printf '\033[36m%s\033[0m\n' "$*"; }
ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }

# The glibc version (major.minor) a system runs: "ldd (GNU libc) 2.41" -> 2.41.
glibc_of() { ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+$'; }

# --- in the container, as root: pin pacman to SteamOS's glibc, then the tools ---------------------------------------
if [ "${1:-}" = "--inside" ]; then
    want=$2   # SteamOS's glibc, major.minor
    major=${want%%.*}
    minor=${want#*.}
    next="$major.$((minor + 1))"
    say "SteamOS has glibc $want; finding the last day Arch's [core] still had it"
    listing=$(curl -fsSL --retry 3 "$archive/packages/g/glibc/") || fail "cannot read $archive (no network?)"
    # The archive's index: one line per file, "<a href="glibc-2.42+r...-1-x86_64.pkg.tar.zst">...</a>  12-Aug-2025 07:31  ..."
    first_next=$(grep -E "href=\"glibc-${next//./\\.}[+-][^\"]*-x86_64\\.pkg\\.tar\\.zst\"" <<<"$listing" |
                 grep -oE '[0-9]{2}-[A-Z][a-z]{2}-[0-9]{4}' |
                 while read -r day; do date -u -d "${day//-/ }" +%s; done | sort -n | head -1)
    if [ -n "$first_next" ]; then
        # glibc $next reached the archive (in [core-testing] first) on that day: [core] still had $want two days
        # before.
        snapshot=$(date -u -d "@$((first_next - 2 * 86400))" +%Y/%m/%d)
        say "pinning pacman to the Arch Linux Archive of $snapshot"
        # shellcheck disable=SC2016  # $repo and $arch are pacman's, not the shell's
        printf 'Server = %s/repos/%s/$repo/os/$arch\n' "$archive" "$snapshot" >/etc/pacman.d/mirrorlist
    else
        say "Arch has no glibc newer than $want yet: today's Arch matches SteamOS"
    fi
    # -uu: down to the snapshot's versions (the container came from today's Arch).
    pacman -Syyuu --noconfirm
    pacman -S --needed --noconfirm "${packages[@]}"
    got=$(glibc_of)
    [ "$got" = "$want" ] ||
        fail "the container has glibc $got, SteamOS $want: what it builds may not load on SteamOS (tell Claude)"
    ok "container glibc $got = SteamOS's"
    exit 0
fi

recreate=0
case "${1:-}" in
    "") ;;
    --recreate) recreate=1 ;;
    *) sed -n '2,18p' "$0"; exit 2 ;;
esac

# --- on SteamOS ---------------------------------------------------------------------------------------------------
if [ -n "${CONTAINER_ID:-}" ] || [ -e /run/.containerenv ]; then
    fail "run this on SteamOS itself (Konsole), not inside a container"
fi
command -v distrobox >/dev/null 2>&1 ||
    fail "distrobox not found: it comes with SteamOS 3.5 and later (Settings > System > Software updates)"
command -v podman >/dev/null 2>&1 || fail "podman not found: it comes with distrobox on SteamOS 3.5 and later"
host_glibc=$(glibc_of)
[ -n "$host_glibc" ] || fail "cannot tell this system's glibc version (ldd --version)"

exists() { distrobox list --no-color 2>/dev/null | awk -F'|' 'NR > 1 { gsub(/ /, "", $2); print $2 }' | grep -qx "$container"; }

if [ "$recreate" = 1 ] && exists; then
    say "removing the container $container"
    distrobox rm --force "$container"
fi
if exists; then
    ok "container $container is there"
else
    say "making the container $container from $image (downloads ~150 MB the first time)"
    distrobox create --yes --name "$container" --image "$image"
fi

say "installing the build tools in $container (the first time: several minutes, ~1 GB)"
# The first `enter` also sets the container up for distrobox; sudo needs no password inside it.
distrobox enter "$container" -- sudo bash "$(cd "$(dirname "$0")" && pwd)/$(basename "$0")" --inside "$host_glibc"

say "what the container has:"
# shellcheck disable=SC2016  # expanded in the container, not here
distrobox enter "$container" -- sh -c \
    'gcc --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"; git --version;
     x86_64-w64-mingw32-g++ --version | head -1; i686-w64-mingw32-g++ --version | head -1; ldd --version | head -1'

ok "ready. Build and install WheelBri with: bash scripts/linux/deck-build.sh"
if [ ! -e /etc/udev/rules.d/70-wheelbri.rules ]; then
    say "once, after the first build: ~/wheelbri/install-udev-rules.sh (asks for your sudo password; set one with passwd)"
fi
