// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// DirectInput force feedback parameters -> the common FfbEffect model. Kept free of <dinput.h> (plain
// numbers in DirectInput's units) so it is unit-tested on every platform; the shim unpacks DIEFFECT.
//
// DirectInput units: magnitudes, offsets and coefficients -10000..10000, saturations, dead bands and
// gains 0..10000, phase in hundredths of a degree, period in microseconds.

#include "wheelbri/Ffb.hpp"
#include "wheelbri/FfbShape.hpp"

#include <cstdint>

namespace wheelbri::di {

// Largest magnitude, offset, coefficient, saturation or gain still read as a (clamped) value: ten times
// DirectInput's full scale. Anything beyond is not a force the game meant: GRID (2008) passes
// lMagnitude = -2147483648 (what x86 makes of a float that is NaN or out of range) and that must not
// become a full push. Such values give 0 instead.
inline constexpr std::int64_t kGarbageAbove = 100000;

[[nodiscard]] constexpr bool isGarbage(const std::int64_t value) {
    return value > kGarbageAbove || value < -kGarbageAbove;
}

// DIEFF_* coordinate systems.
inline constexpr std::uint32_t kCartesian = 0x10;
inline constexpr std::uint32_t kPolar = 0x20;
inline constexpr std::uint32_t kSpherical = 0x40;

// How much of an effect's direction lies along the wheel's axis (X), -1..1. `coordinates` holds the
// DIEFF_* flags, `firstDirection` is rglDirection[0] (0 when the effect gives none).
[[nodiscard]] float directionAlongX(std::uint32_t coordinates, std::int32_t firstDirection);

[[nodiscard]] FfbEffect constantForce(std::int32_t magnitude, float direction, std::uint32_t gain);

// DICONDITION for the wheel axis. `type` is Spring, Damper, Friction or Inertia.
[[nodiscard]] FfbEffect condition(FfbEffectType type, std::int32_t offset, std::int32_t positiveCoefficient,
                                  std::int32_t negativeCoefficient, std::uint32_t positiveSaturation,
                                  std::uint32_t negativeSaturation, std::int32_t deadBand, std::uint32_t gain);

// DIPERIODIC. `type` is Sine, Square, Triangle, SawtoothUp or SawtoothDown. A negative direction turns
// the wave round (phase + 180 degrees) and flips the offset.
[[nodiscard]] FfbEffect periodic(FfbEffectType type, std::uint32_t magnitude, std::int32_t offset, std::uint32_t phase,
                                 std::uint32_t periodUs, float direction, std::uint32_t gain);

// DIENVELOPE: levels 0..10000, times in microseconds.
[[nodiscard]] FfbEnvelope envelope(std::uint32_t attackLevel, std::uint32_t attackTimeUs, std::uint32_t fadeLevel,
                                   std::uint32_t fadeTimeUs);

}  // namespace wheelbri::di
