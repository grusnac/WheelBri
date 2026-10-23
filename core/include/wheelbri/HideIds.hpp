#pragma once

#include "wheelbri/MapperConfig.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::hideids {

// The rig's HID devices as Windows names them, for the hide filter (wheelbri-hide): which ones may get it, and the
// ids the service, the helper and install-hide.ps1 pass around. Portable and unit-tested.

// The USB ids in a HID device instance id ("HID\\VID_346E&PID_0004&MI_02\\8&...", any case): 346e:0004. Nullopt
// for anything else. How the service and wheelbri-hide's installer find a rig's devices among Windows' devices.
[[nodiscard]] std::optional<DeviceId> hidDeviceIdOf(std::string_view instanceId);

// Whether a rig device may get the hiding filter: never a virtual wheel (046d:c262, 046d:c299: ours; hiding it would
// hide what games are meant to see) and never an empty id.
[[nodiscard]] bool mayHide(const DeviceId& id);

// Whether a HID collection is a game controller (what DirectInput and games open), from its compatible ids:
// joystick, gamepad or multi-axis controller (usage page 1, usages 4, 5, 8; Windows adds HID_DEVICE_SYSTEM_GAME for
// the first two only, and many wheels are multi-axis controllers) or any simulation control
// (usage page 2). A vendor tool's collections (vendor-defined pages) are not.
[[nodiscard]] bool isGameController(const std::vector<std::string>& compatibleIds);

// Why a HID collection must never get the hide filter, from its compatible ids: a keyboard, keypad, mouse or pointer,
// system controls (power, sleep), consumer controls (media keys) or a digitizer (pen, touch screen). Empty when none of
// those (a game controller, or compatible ids Windows did not give). The WheelBri Helper checks every interface it is
// asked to hide against this, whatever the app said.
[[nodiscard]] std::string neverHiddenUsage(const std::vector<std::string>& compatibleIds);

// The device instance id behind a HID interface path as DirectInput reports it (DIPROP_GUIDANDPATH):
// "\\?\hid#vid_346e&pid_0005&mi_02#8&8052f69&0&0000#{4d1e55b2-...}" -> "HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000"
// (upper case; Windows compares them ignoring case). Empty if it is not a HID path.
[[nodiscard]] std::string instanceIdOfHidPath(std::string_view path);

// "346e:0004" (the spelling install-hide.ps1 -Devices takes).
[[nodiscard]] std::string deviceIdText(const DeviceId& id);

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b);
[[nodiscard]] bool containsIgnoreCase(std::string_view text, std::string_view part);

}  // namespace wheelbri::hideids
