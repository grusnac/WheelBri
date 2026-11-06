// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Ffb.hpp"

namespace wheelbri {

// The centring spring of a target whose own Windows software had one (DeviceIdentity::centringSpring: the G25 and
// G27). Logitech Profiler and Logitech Gaming Software had "Enable Centering Spring in Force Feedback Games" with a
// "Centering Spring Strength" of 0 to 100% (its registry value 0 to 10000): a spring around the centre that the driver
// played in every force feedback game, on top of the game's own forces, whatever the game sent. Race Driver: GRID
// sends only a constant force and turns the wheel's own auto-centre off (DIPROP_AUTOCENTER 0), so without it the
// wheel stays wherever it was left at standstill and in the menus.
//
// WheelBri's: [ffb] centring_spring = 0 to 100 (percent; 0 = off, as the checkbox unticked) in the target's mapping.
// FfbController plays it in a slot of its own (IFfbSink::kCentringSpringSlot) through its own FfbSafetyLimiter while
// the game's force feedback is open, from Start to Stop.
inline constexpr float kCentringSpringMaxPercent = 100.0F;

// The spring for `percent` (clamped to 0..100; not a number: 0): centred, no dead band, both coefficients
// percent / 100 (100% = the full force at the end of the wheel's travel, as a DirectInput spring of 10000), full
// saturation (the limiter's cap then holds it).
[[nodiscard]] FfbEffect centringSpringEffect(float percent);

}  // namespace wheelbri
