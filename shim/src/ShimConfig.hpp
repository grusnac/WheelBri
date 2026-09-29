#pragma once

#include "wheelemu/EmulatedJoystick.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace wheelemu::shim {

// Settings of the proxy DLL, read once from wheelemu-shim.ini next to the DLL. The file is optional;
// without it the shim presents the first Moza device as a G920 fed by the service on the default port.
//
//   [shim]
//   emulate = true             ; false = Phase 0 behaviour: log and pass every device through unchanged
//   source_vid = 346e          ; USB vendor id of the physical wheel to replace (hex)
//   source_pid = any           ; or a hex product id
//   target_profile = g920      ; g920 or g29, must match the service's target_profile
//   state_port = 41329         ; must match the service's state_port
//   stale_after_ms = 250       ; no packet for this long = wheel centred, pedals released
//   force_feedback = true      ; false = the emulated wheel reports no force feedback at all
//   throttle_axis = slider     ; DirectInput axis per pedal: x y z rx ry rz slider (clutch also: none).
//   brake_axis = y             ;   Defaults come from target_profile: g920 = slider / y / rz, g29 = y / rz / slider.
//   clutch_axis = rz
//   invert_pedals = false      ; true = pressed and released swapped for all pedals
//
// Force feedback is passed straight to the physical wheel's own driver in Phase 2, so its strength is
// limited only by the wheel's settings (Moza Pit House), not by the service's [ffb] limiter.
struct ShimConfig {
    bool emulate = true;
    std::uint16_t sourceVendorId = 0x346E;
    std::optional<std::uint16_t> sourceProductId;
    std::string targetProfile = "g920";
    std::uint16_t statePort = 41329;
    int staleAfterMs = 250;
    bool forceFeedback = true;
    std::optional<ObjectGuid> throttleAxis;     // overrides of the profile's layout
    std::optional<ObjectGuid> brakeAxis;
    std::optional<ObjectGuid> clutchAxis;
    bool invertPedals = false;

    // The profile's layout with the overrides applied (validated when the file is loaded).
    [[nodiscard]] JoystickLayout layout(std::size_t buttonCount) const {
        JoystickLayout result = layoutFor(targetProfile, buttonCount);
        result.throttle = throttleAxis.value_or(result.throttle);
        result.brake = brakeAxis.value_or(result.brake);
        result.clutch = clutchAxis.value_or(result.clutch);
        result.invertPedals = invertPedals;
        return result;
    }

    [[nodiscard]] bool matchesSource(std::uint16_t vendorId, std::uint16_t productId) const {
        return emulate && vendorId == sourceVendorId && (!sourceProductId || *sourceProductId == productId);
    }
};

// Loads the file on first use (never from DllMain) and logs what it used.
[[nodiscard]] const ShimConfig& shimConfig();

}  // namespace wheelemu::shim
