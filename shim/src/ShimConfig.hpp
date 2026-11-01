#pragma once

#include "wheelbri/EmulatedJoystick.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::shim {

// Settings of the proxy DLL, read once from wheelbri-shim.ini next to the DLL. The file is optional;
// without it the shim presents the first device with vendor id 346e (source_vid) as a G920 fed by the service on the
// default port.
//
//   [shim]
//   emulate = true             ; false = Phase 0 behaviour: log and pass every device through unchanged
//   logi_log = false           ; true = log the game's calls into Logitech's SDK DLLs (LogiHook.hpp)
//   source_vid = 346e          ; USB vendor id of the rig's wheel to replace (hex, the Rig tab's vid:pid)
//   source_pid = any           ; or a hex product id
//   target_profile = g920      ; g25, g27, g920 or g923 (the app sets it at Start, from the Mapping tab's Target)
//   state_port = 41329         ; must match the service's state_port
//   stale_after_ms = 1500      ; no packet for this long = wheel centred, pedals released (the last
//                              ; input is held until then, so a short service hiccup is not felt)
//   force_feedback = service   ; service = game effects go to wheelbri-service (safety limiter, watchdog);
//                              ; passthrough = straight to the wheel's driver; off = no force feedback
//   ffb_port = 41330           ; must match ffb_port in the service config
//   throttle_axis = y          ; DirectInput axis per pedal: x y z rx ry rz slider (clutch also: none).
//   brake_axis = rz            ;   Defaults: the target's: y / rz / slider (the G25's clutch: z).
//   clutch_axis = slider
//   invert_pedals = false      ; true = pressed and released swapped for all pedals
//   chain = dinput8-chain.dll  ; another mod's dinput8.dll the app chained: DirectInput goes to it (it goes on to
//                              ; Windows'); unset = straight to Windows' own (validChainName, core ShimGames.hpp)
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
    // [shim] logi_log = true: log every call the game makes into Logitech's SDK DLLs (LogiHook.hpp). A debugging aid.
    bool logiLog = false;
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
    // [shim] game_axes = wheelbri: the app wrote the axes above for this game (its profile's and the user's own, the
    // Games tab; core ShimGames setShimGameAxes): the game's profile is not laid over them again.
    bool gameAxesFromApp = false;
    // [shim] chain: the DLL in the game's folder DirectInput goes to (RealDInput.hpp); empty = Windows' own.
    std::string chain;
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

// Whether the game's profile (`game`: the one for the game's program, null when it has none) is laid over `config`'s
// layout, and why not: the app already wrote the game's axes (game_axes = wheelbri), the profile is for another target,
// or with this file's axes it would put two pedals on one axis or a pedal on x.
enum class GameProfileUse { NoProfile, AxesFromApp, OtherTarget, Clashes, Used };
[[nodiscard]] GameProfileUse gameProfileUse(const ShimConfig& config, const GameProfile* game);

// Loads the file on first use (never from DllMain) and logs what it used.
[[nodiscard]] const ShimConfig& shimConfig();

// The settings in `text` (the file's contents) over the defaults; each value it cannot use is added to `problems`
// and its default kept. What shimConfig() reads the file with.
[[nodiscard]] ShimConfig parseShimConfig(const std::string& text, std::vector<std::string>& problems);

}  // namespace wheelbri::shim
