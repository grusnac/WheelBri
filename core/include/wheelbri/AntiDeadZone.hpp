// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

// Anti-dead zone: for games with a steering dead zone of their own around the centre (NFS Underground, Most Wanted:
// the car goes straight for the first part of the turn each way, whatever the wheel sends; measured 2026-11-11, the
// published steering linear and the dead zone still there without any mod). As soon as the wheel leaves the centre,
// the steering the game reads starts at `percent` of full lock and the rest of the turn is scaled to fit, so the
// game's dead zone ends where the wheel leaves the centre. Set it at or just under the game's dead zone: above it the
// car twitches when the wheel crosses the centre. 0 = off.
//
// [steering] anti_deadzone (the Mapping tab, every game); a game's profile ([steering] anti_deadzone) and the user's
// value for it ([game_tuning] <game>.anti_deadzone) win while that game runs. Applied by the service to what the game
// reads only (Pipeline::setAntiDeadZone): the window and telemetry's steer show the wheel as mapped, out_steer what the
// game got. Not a dead zone or a curve of WheelBri's own: it makes up for the game's.
namespace wheelbri {

inline constexpr float kAntiDeadZoneMaxPercent = 50.0F;

// `steering` -1..1 as mapped; `percent` 0..kAntiDeadZoneMaxPercent (outside: clamped; not a number: off).
[[nodiscard]] inline float withAntiDeadZone(const float steering, const float percent) {
    if (!(percent > 0.0F) || steering == 0.0F || !std::isfinite(steering)) return steering;
    const float start = std::min(percent, kAntiDeadZoneMaxPercent) / 100.0F;
    return std::copysign(start + (1.0F - start) * std::min(std::fabs(steering), 1.0F), steering);
}

// Whether a value read from a config, a profile or typed in is in range (NaN is not).
[[nodiscard]] inline bool antiDeadZoneUsable(const float percent) {
    return percent >= 0.0F && percent <= kAntiDeadZoneMaxPercent;
}

}  // namespace wheelbri
