#pragma once

// DirectInput force feedback parameters -> the common FfbEffect model. Kept free of <dinput.h> (plain
// numbers in DirectInput's units) so it is unit-tested on every platform; the shim unpacks DIEFFECT.
//
// DirectInput units: magnitudes, offsets and coefficients -10000..10000, saturations, dead bands and
// gains 0..10000, phase in hundredths of a degree, period in microseconds.

#include "wheelbri/Ffb.hpp"

#include <cstdint>

namespace wheelbri::di {

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

}  // namespace wheelbri::di
