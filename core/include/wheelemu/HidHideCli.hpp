#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wheelemu::hidhide {

// Reading what HidHide's command-line tool (HidHideCLI.exe, https://github.com/nefarius/HidHide) prints, so the
// service can hide the real wheel from games while it presents the virtual G920 (the same steps as
// tools/vbridge/HidHide.cs). Portable and unit-tested; running the tool is in service/src/HidHide.cpp.

// Device instance paths of the game-controller interfaces of devices with `vendorId` (e.g. "346E") in the JSON
// of `--dev-gaming`: [ { "friendlyName": ..., "devices": [ { "gamingDevice": true,
// "deviceInstancePath": "HID\\VID_346E&PID_0005&MI_02\\...", ... } ] } ]. Only game-controller interfaces, so a
// vendor tool (Moza Pit House) keeps its own interfaces. Empty if the JSON cannot be read.
[[nodiscard]] std::vector<std::string> gamingDevicesOf(std::string_view json, std::string_view vendorId);

// Every device of `vendorId` in that JSON, described for the log ("PATH (gamingDevice false, present true)").
[[nodiscard]] std::vector<std::string> describeDevicesOf(std::string_view json, std::string_view vendorId);

// The quoted values of `--dev-list` / `--app-list` lines (--dev-hide "PATH" / --app-reg "PATH").
[[nodiscard]] std::vector<std::string> quotedValues(std::string_view output);

// The entries of `stored` equal to one of `wanted` ignoring case, spelled as stored (HidHide's --dev-unhide only
// removes an exact match).
[[nodiscard]] std::vector<std::string> storedSpellings(const std::vector<std::string>& stored,
                                                       const std::vector<std::string>& wanted);

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b);
[[nodiscard]] bool containsIgnoreCase(std::string_view text, std::string_view part);

}  // namespace wheelemu::hidhide
