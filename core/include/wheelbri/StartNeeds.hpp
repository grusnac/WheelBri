#pragma once

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/Rig.hpp"

#include <cstdint>
#include <string>
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
    // Set when no wheel can be named (no rig yet, or a rig with no device): Start cannot run either.
    std::string problem;
};

// A game controller connected now, as Windows (or evdev) lists it.
struct PresentDevice {
    std::string name;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
};

// `rig`: the active rig, or nullptr when there is none. The wheel is the config's source_device when it names one,
// else the rig's wheel (the device with a Steering axis, else its first), as a session picks it.
[[nodiscard]] StartNeeds startNeeds(const EmulatorConfig& config, const Rig* rig);

// The labels of the needed devices that are not connected, in order.
[[nodiscard]] std::vector<std::string> missingDevices(const StartNeeds& needs, const std::vector<PresentDevice>& present);

}  // namespace wheelbri
