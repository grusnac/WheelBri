// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/Rig.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// Which of the rig's devices a session needs before Start may run: the wheel and every accessory the mapping takes an
// axis or button from. Devices of the rig the mapping does not use are not needed. The app's Mapping tab greys Start
// out and says what is missing (in red) until all of them are connected.
struct NeededDevice {
    std::string label;      // what the user calls it: the rig's name, else its vid:pid (or the config's text)
    std::string match;      // the wheel from the config's source_device (part of a name or vid:pid); else empty
    DeviceId id;            // an accessory, or the rig's wheel: its exact id
};

struct StartNeeds {
    std::vector<NeededDevice> devices;   // the wheel first, then the accessories
    // Set when Start cannot run, saying what is missing: no rig yet (kNoRig), a rig with no device, or a rig the mapping
    // takes nothing from (kNoMapping: not one of the target's axes, buttons or D-pad directions comes from a device of
    // the rig).
    std::string problem;
};

inline constexpr const char* kNoRig = "NO RIG: set it up in the Rig tab";
inline constexpr const char* kNoMapping = "NO MAPPING: map the rig in the Mapping tab";

// Whether at least one of the target's controls (steering, a pedal, a button, a D-pad direction) is taken from a
// device of `rig`; `wheelInRig`: the mapping's device 0 (the wheel) is one of the rig's devices.
[[nodiscard]] bool mappingUsesRig(const MapperConfig& mapper, const Rig& rig, bool wheelInRig);

// A game controller connected now, as Windows (or evdev) lists it.
struct PresentDevice {
    std::string name;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
};

// `rig`: the active rig, or nullptr when there is none. The wheel is the config's source_device when it names one,
// else the rig's wheel (the device with a Steering axis, else its first), as a session picks it.
[[nodiscard]] StartNeeds startNeeds(const EmulatorConfig& config, const Rig* rig);

// Why the config cannot run as `target` (the Mapping tab's Target: "g920", "g25"), or "" when it can: its
// target_profile is a wheel WheelBri does not emulate, or another target than the one picked (a user's own mapping
// edited by hand). The session encodes for the config's target and the shim or virtual device presents the picked one,
// so a mismatch would give a game one wheel's name with another's buttons.
[[nodiscard]] std::string targetProblem(const EmulatorConfig& config, std::string_view target);

// The labels of the needed devices that are not connected, in order.
[[nodiscard]] std::vector<std::string> missingDevices(const StartNeeds& needs, const std::vector<PresentDevice>& present);

}  // namespace wheelbri
