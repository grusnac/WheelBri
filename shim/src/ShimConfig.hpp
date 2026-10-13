#pragma once

#include "wheelbri/EmulatedJoystick.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace wheelbri::shim {

// Settings of the proxy DLL, read once from wheelbri-shim.ini next to the DLL. The file is optional;
// without it the shim presents the first device with vendor id 346e (source_vid) as a G920 fed by the service on the
// default port.
//
//   [shim]
//   emulate = true             ; false = Phase 0 behaviour: log and pass every device through unchanged
//   source_vid = 346e          ; USB vendor id of the rig's wheel to replace (hex, the Rig tab's vid:pid)
//   source_pid = any           ; or a hex product id
//   target_profile = g920      ; g920 or g25 (the app sets it at Start, from the Mapping tab's Target)
//   state_port = 41329         ; must match the service's state_port
//   stale_after_ms = 1500      ; no packet for this long = wheel centred, pedals released (the last
//                              ; input is held until then, so a short service hiccup is not felt)
//   force_feedback = service   ; service = game effects go to wheelbri-service (safety limiter, watchdog);
//                              ; passthrough = straight to the wheel's driver; off = no force feedback
//   ffb_port = 41330           ; must match ffb_port in the service config
//   throttle_axis = y          ; DirectInput axis per pedal: x y z rx ry rz slider (clutch also: none).
//   brake_axis = rz            ;   Defaults: the target's, y / rz / slider for the G920 and the G25.
//   clutch_axis = slider
//   invert_pedals = false      ; true = pressed and released swapped for all pedals
//
// The pedal axes are a sieve of three layers: the target's defaults, then any *_axis set here, then the game profile of
// the game the shim is loaded into, if it has one for this target (GameProfiles.hpp, config/games/*.ini): what the game
// needs changed, and nothing else.
//
// In passthrough mode force feedback is limited only by the wheel's own settings (its maker's software), not
// by the service's [ffb] limiter; service mode is the default and the safe one.
enum class FfbMode { Service, Passthrough, Off };

[[nodiscard]] const char* toString(FfbMode mode);

struct ShimConfig {
    bool emulate = true;
    std::uint16_t sourceVendorId = 0x346E;
    std::optional<std::uint16_t> sourceProductId;
    std::string targetProfile = "g920";
    std::uint16_t statePort = 41329;
    int staleAfterMs = 1500;
    FfbMode forceFeedback = FfbMode::Service;
    std::uint16_t ffbPort = 41330;
    std::optional<ObjectGuid> throttleAxis;     // overrides of the profile's layout
    std::optional<ObjectGuid> brakeAxis;
    std::optional<ObjectGuid> clutchAxis;
    bool invertPedals = false;
    // The game profile of the game the shim is in (gameProfiles(), never freed), when it has one for targetProfile and
    // its layout is valid; null otherwise.
    const GameProfile* gameProfile = nullptr;

    // The layout: the target's, the overrides of this file on top, the game profile on top of that (validated when the
    // file is loaded).
    [[nodiscard]] JoystickLayout layout(std::size_t buttonCount) const {
        JoystickLayout result = layoutFor(targetProfile, buttonCount);
        result.throttle = throttleAxis.value_or(result.throttle);
        result.brake = brakeAxis.value_or(result.brake);
        result.clutch = clutchAxis.value_or(result.clutch);
        result.invertPedals = invertPedals;
        return gameProfile != nullptr ? overlay(result, *gameProfile) : result;
    }

    [[nodiscard]] bool matchesSource(std::uint16_t vendorId, std::uint16_t productId) const {
        return emulate && vendorId == sourceVendorId && (!sourceProductId || *sourceProductId == productId);
    }
};

// Loads the file on first use (never from DllMain) and logs what it used.
[[nodiscard]] const ShimConfig& shimConfig();

}  // namespace wheelbri::shim
