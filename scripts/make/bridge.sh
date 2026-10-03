#!/usr/bin/env bash
# Build the C# tools in tools/vbridge: the HIDMaestro bridge (needs third_party/hidmaestro/HIDMaestro.Core.dll)
# and the probes. Needs the .NET 10 SDK.
#
#   scripts/make/bridge.sh            bridge + FfbProbe + ClutchProbe + SelfTest
#   scripts/make/bridge.sh --clean    rebuild them from scratch

source "$(dirname "$0")/common.sh"

dotnet=$(dotnet_exe) || fail "dotnet not found: install the .NET 10 SDK"
extra=()
[ "${1:-}" = "--clean" ] && extra=(--no-incremental)

for project in tools/vbridge/wheelbri-bridge.csproj tools/vbridge/FfbProbe tools/vbridge/ClutchProbe \
               tools/vbridge/SelfTest; do
    say "building $project..."
    "$dotnet" build "$project" -c Release -v q -nologo "${extra[@]}" || fail "$project failed"
done
ok "built: tools/vbridge/bin/Release/net10.0-windows10.0.26100.0/wheelbri-bridge.exe and the probes"
