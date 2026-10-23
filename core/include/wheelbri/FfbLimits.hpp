#pragma once

// How strong the force feedback is, is set in the wheel's own app (Moza Pit House, Fanatec's tuning menu,
// Simucube True Drive, Thrustmaster's control panel, Logitech G HUB), the same for every game. WheelBri passes
// all of what the game asks: gain 1, cap 1. Its limiter still guards against jolts (slew), a game that
// stopped talking (watchdog), a push left on (frozen force fade) and values that are not forces (DiEffect).
//
// Configs written before this kept WheelBri's own strength (gain 0.7, cap 0.8, exactly those two): the Mapping tab
// moves a user's own config to the new values when it opens it (movedFromOldLimits), and says so. Any other values
// are the user's own choice and stay.

#include "wheelbri/Ffb.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wheelbri {

inline constexpr float kWheelAppMasterGain = 1.0F;
inline constexpr float kWheelAppMaxMagnitude = 1.0F;
inline constexpr float kOldMasterGain = 0.7F;
inline constexpr float kOldMaxMagnitude = 0.8F;

// The INI text with [ffb] master_gain = 1.0 and max_magnitude = 1.0; every other line as it was.
[[nodiscard]] std::string withWheelAppLimits(std::string_view iniText);

// The INI text moved to the new limits when its [ffb] has the older ones (master_gain 0.7 and max_magnitude 0.8):
// [ffb] 1 / 1, and a game's ffb_strength 0.7 or ffb_cap 0.8 in [game_tuning] (the older limits repeated) dropped.
// nullopt when [ffb] has anything else (or no values): the user's own choice, left alone.
[[nodiscard]] std::optional<std::string> movedFromOldLimits(std::string_view iniText);

}  // namespace wheelbri
