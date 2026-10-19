#!/usr/bin/env bash
# Build (and optionally install) the UMDF drivers from the MSYS2 shell: runs drivers.ps1 in PowerShell.
# Needs Visual Studio with the WDK (README.md, "Drivers"). Installing asks for Administrator.
#
#   scripts/make/drivers.sh                     build both (wheel, hide), Release
#   scripts/make/drivers.sh -Only wheel         build one (wheel or hide; wheel: the virtual wheel, all targets)
#   scripts/make/drivers.sh -Install            build, then install (Administrator prompt)
#   scripts/make/drivers.sh -Clean              rebuild from scratch
#   scripts/make/drivers.sh -Configuration Debug

source "$(dirname "$0")/common.sh"

on_windows || fail "the drivers are Windows only"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$root/scripts/make/drivers.ps1")" "$@"
