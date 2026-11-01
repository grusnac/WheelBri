#!/usr/bin/env bash
# Remove everything WheelBri has put on this PC (drivers, virtual devices, certificate, install, shortcuts, registry
# entries, data), for a fresh install from scratch: runs clean-pc.ps1 in PowerShell. It lists what it found and asks
# before removing anything; Administrator prompt. The repository (build/, dist/) is not touched.
#
#   scripts/make/clean-pc.sh                                   list, ask, remove
#   scripts/make/clean-pc.sh -KeepData                         keep rigs, configs, logs and telemetry
#   scripts/make/clean-pc.sh -GameFolder "D:\Games\Need for Speed"   also take the shim out of that game folder
#   scripts/make/clean-pc.sh -KeepThirdParty                   keep HidHide and HIDMaestro (removed by default)
#   scripts/make/clean-pc.sh -Yes                              no question

# shellcheck source=common.sh
source "$(dirname "$0")/common.sh"

on_windows || fail "clean-pc is Windows only (MSYS2 UCRT64 shell)"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$root/scripts/make/clean-pc.ps1")" "$@" \
    || fail "the PC was not cleaned completely (see the PowerShell window and its log)"
