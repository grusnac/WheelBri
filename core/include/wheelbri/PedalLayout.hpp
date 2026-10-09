#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <cstdint>
#include <string_view>

namespace wheelbri {

// Where a game reads each pedal of the emulated G920. The device itself reports like a real G920 (wheel X, throttle Y,
// brake Rz, clutch Slider; HidG920.hpp), and every consumer (the driver, the shim, wheelbri-uhid) packs a
// TargetReport's throttle, brake and clutch that way. A game that reads a G920's pedals from other axes gets them
// moved in the service before the state leaves it (KnownGames.hpp detects the game), so the user's mapping is the same
// in every game: the throttle pedal is the throttle, the brake the brake, the clutch the clutch.
enum class PedalLayout : std::uint8_t {
    Logitech,   // a real G920: throttle Y, brake Rz, clutch Slider (NFS Unbound, Heat, Payback; any game not listed)
    Nfs2015,    // NFS 2015 reads a G920's brake from Y, its clutch from Rz and its throttle from the Slider
};

// "Logitech G920", "NFS 2015": for the log and the window.
[[nodiscard]] std::string_view pedalLayoutName(PedalLayout layout);

// `report` with its pedals moved so that, packed as a real G920, the game finds each pedal where it reads it.
// Allocation-free (called on the service loop).
[[nodiscard]] TargetReport applyPedalLayout(const TargetReport& report, PedalLayout layout);

}  // namespace wheelbri
