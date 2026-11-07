#!/usr/bin/env bash
# Once, on the Steam Deck (Desktop Mode, Konsole), from the WheelBri checkout: the tools to build WheelBri there.
#
# SteamOS is Arch Linux underneath, but keeps its own system read-only and has no compiler. So the build runs in an
# Arch Linux distrobox container (distrobox and podman come with SteamOS 3.5 and later; nothing on SteamOS itself
# changes):
#   wheelbri-build   Arch Linux with base-devel (GCC), CMake, Ninja, git, libx11 (the app's window) and mingw-w64-gcc
#                    (the game shim, dinput8.dll: 64-bit and 32-bit, both in that package), from pacman.
# The container is pinned to the last day Arch still shipped SteamOS's own glibc version, so the programs it builds
# load on SteamOS itself, outside the container (today's Arch is usually ahead of SteamOS): it is made from Docker
# Hub's dated Arch image of that week, and pacman reads the Arch Linux Archive of that day. Going down from today's
# Arch instead does not work (packages split since, keys disabled since).
# Your home folder is shared with it: the checkout and ~/wheelbri are the same files inside and out.
#
# Then, after every `git pull`: scripts/linux/deck-build.sh
# After a SteamOS update that brings a newer glibc, deck-build.sh says so: run this again (it makes the container again
# from the matching image).
#
# usage: scripts/linux/deck-setup.sh [--recreate]
#   --recreate   remove the container and make it again (broken; your files are not in it)

set -eu

container=wheelbri-build
archive=https://archive.archlinux.org
hub=https://hub.docker.com/v2/repositories/library/archlinux/tags
packages=(base-devel cmake ninja git libx11 mingw-w64-gcc)

say()  { printf '\033[36m%s\033[0m\n' "$*"; }
ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
fail() { printf '\033[31m%s\033[0m\n' "$*"; exit 1; }

# The glibc version (major.minor) a system runs: "ldd (GNU libc) 2.41" -> 2.41.
glibc_of() { ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+$'; }

# --- in the container, as root: pacman on the snapshot, then the tools ----------------------------------------------
if [ "${1:-}" = "--inside" ]; then
    want=$2       # SteamOS's glibc, major.minor
    snapshot=$3   # YYYY/MM/DD, or "latest"
    if [ "$snapshot" != latest ]; then
        say "pacman reads the Arch Linux Archive of $snapshot"
        # shellcheck disable=SC2016  # $repo and $arch are pacman's, not the shell's
        printf 'Server = %s/repos/%s/$repo/os/$arch\n' "$archive" "$snapshot" >/etc/pacman.d/mirrorlist
        # As in the container's pre-init hook (on SteamOS, below): signatures checked, trust level not.
        sed -i 's/^SigLevel *=.*/SigLevel = Required DatabaseOptional TrustAll/' /etc/pacman.conf
    fi
    # The image is from the snapshot's week (at most a few days older): -Syyu brings it up to the snapshot.
    pacman -Syyu --noconfirm
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
    *) sed -n '2,20p' "$0"; exit 2 ;;
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

# The snapshot: the last day Arch's [core] still had SteamOS's glibc.
major=${host_glibc%%.*}
minor=${host_glibc#*.}
next="$major.$((minor + 1))"
say "SteamOS has glibc $host_glibc; finding the last day Arch's [core] still had it"
listing=$(curl -fsSL --retry 3 "$archive/packages/g/glibc/") || fail "cannot read $archive (no network?)"
# The archive's index: one line per file, with the "+" URL-encoded in the link:
#   <a href="glibc-2.42%2Br17%2Bg...-1-x86_64.pkg.tar.zst">glibc-2.42+r17+g...</a>  13-Aug-2025 15:46  10M
grep -qE 'href="glibc-[0-9]+\.[0-9]+' <<<"$listing" || fail "cannot read the glibc list at $archive (format changed?)"
first_next=$(grep -E "href=\"glibc-${next//./\\.}(\\+|%2B|-)[^\"]*-x86_64\\.pkg\\.tar\\.zst\"" <<<"$listing" |
             grep -oE '[0-9]{2}-[A-Z][a-z]{2}-[0-9]{4}' |
             while read -r day; do date -u -d "${day//-/ }" +%s; done | sort -n | head -1)
if [ -n "$first_next" ]; then
    # glibc $next reached the archive (in [core-testing] first) on that day: [core] still had $host_glibc two days
    # before.
    snap_s=$((first_next - 2 * 86400))
    snapshot=$(date -u -d "@$snap_s" +%Y/%m/%d)
    day=$(date -u -d "@$snap_s" +%Y%m%d)
    year=${day:0:4}
    # Docker Hub's weekly Arch images are tagged base-YYYYMMDD.0.N: the newest one not after the snapshot.
    tag=$(for y in "$year" "$((year - 1))"; do curl -fsSL --retry 3 "$hub?name=base-$y&page_size=100" || true; done |
          grep -oE '"name":"base-[0-9]{8}\.[0-9.]+"' | cut -d'"' -f4 |
          awk -v d="$day" 'substr($0, 6, 8) <= d' | sort | tail -1)
    [ -n "$tag" ] || fail "no Arch image on Docker Hub from before $snapshot (no network?)"
    image=docker.io/library/archlinux:$tag
    say "snapshot $snapshot, image $tag"
else
    snapshot=latest
    image=docker.io/library/archlinux:latest
    say "Arch has no glibc newer than $host_glibc yet: today's Arch matches SteamOS"
fi

exists() { distrobox list --no-color 2>/dev/null | awk -F'|' 'NR > 1 { gsub(/ /, "", $2); print $2 }' | grep -qx "$container"; }

if [ "$recreate" = 0 ] && exists; then
    have=$(podman container inspect --format '{{.ImageName}}' "$container" 2>/dev/null || true)
    if [ "$have" != "$image" ]; then
        say "the container $container is from ${have:-another image}, not $image: making it again"
        recreate=1
    fi
fi
if [ "$recreate" = 1 ] && exists; then
    say "removing the container $container"
    distrobox rm --force "$container"
fi
if exists; then
    ok "container $container is there"
else
    say "making the container $container from $image (downloads ~150 MB the first time)"
    hooks=()
    if [ "$snapshot" != latest ]; then
        # distrobox's own first start updates the system with pacman: on the snapshot too, not today's Arch (which
        # the image's keyring no longer trusts, and which would undo the pin). distrobox expands the hook text itself,
        # so pacman's $repo and $arch are written as \044 (a "$" to printf).
        # TrustAll: every package still needs a valid signature from a key in the image's keyring (of the snapshot's
        # week); only the trust level is not asked, because the master keys' signatures on the packagers' keys have
        # expired since (gpg then says "unknown trust" for all of them).
        hooks=(--pre-init-hooks "printf 'Server = $archive/repos/$snapshot/\\044repo/os/\\044arch\\n' >/etc/pacman.d/mirrorlist && sed -i 's/^SigLevel *=.*/SigLevel = Required DatabaseOptional TrustAll/' /etc/pacman.conf")
    fi
    distrobox create --yes --name "$container" --image "$image" "${hooks[@]}"
fi

say "installing the build tools in $container (the first time: several minutes, ~1 GB)"
# The first `enter` also sets the container up for distrobox; sudo needs no password inside it.
distrobox enter "$container" -- sudo bash "$(cd "$(dirname "$0")" && pwd)/$(basename "$0")" --inside "$host_glibc" "$snapshot"

say "what the container has:"
# shellcheck disable=SC2016  # expanded in the container, not here
distrobox enter "$container" -- sh -c \
    'gcc --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"; git --version;
     x86_64-w64-mingw32-g++ --version | head -1; i686-w64-mingw32-g++ --version | head -1; ldd --version | head -1'

ok "ready. Build and install WheelBri with: bash scripts/linux/deck-build.sh"
if [ ! -e /etc/udev/rules.d/70-wheelbri.rules ]; then
    say "once, after the first build: ~/wheelbri/install-udev-rules.sh (asks for your sudo password; set one with passwd)"
fi
