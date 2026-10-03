#pragma once

#include "wheelbri/MapperConfig.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::hidhide {

// Reading what HidHide's command-line tool (HidHideCLI.exe, https://github.com/nefarius/HidHide) prints, so the
// service can hide the real wheel from games while it presents the virtual G920 (the same steps as
// the old bridge's HidHide.cs). Portable and unit-tested.

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

// The USB ids in a HID device instance id ("HID\\VID_346E&PID_0004&MI_02\\8&...", any case): 346e:0004. Nullopt
// for anything else. How the service and wheelbri-hide's installer find a rig's devices among Windows' devices.
[[nodiscard]] std::optional<DeviceId> hidDeviceIdOf(std::string_view instanceId);

// Whether a rig device may get the hiding filter: never the virtual G920 (046d:c262, ours; hiding it would hide
// what games are meant to see) and never an empty id.
[[nodiscard]] bool mayHide(const DeviceId& id);

// Whether a HID collection is a game controller (what DirectInput and games open), from its compatible ids:
// joystick, gamepad or multi-axis controller (usage page 1, usages 4, 5, 8; Windows adds HID_DEVICE_SYSTEM_GAME for
// the first two only, and many wheels, the Moza R3 among them, are multi-axis controllers) or any simulation control
// (usage page 2). A vendor tool's collections (vendor-defined pages) are not.
[[nodiscard]] bool isGameController(const std::vector<std::string>& compatibleIds);

// The device instance id behind a HID interface path as DirectInput reports it (DIPROP_GUIDANDPATH):
// "\\?\hid#vid_346e&pid_0005&mi_02#8&8052f69&0&0000#{4d1e55b2-...}" -> "HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000"
// (upper case; Windows compares them ignoring case). Empty if it is not a HID path.
[[nodiscard]] std::string instanceIdOfHidPath(std::string_view path);

// "346e:0004" (the spelling install-hide.ps1 -Devices takes).
[[nodiscard]] std::string deviceIdText(const DeviceId& id);

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b);
[[nodiscard]] bool containsIgnoreCase(std::string_view text, std::string_view part);

}  // namespace wheelbri::hidhide
