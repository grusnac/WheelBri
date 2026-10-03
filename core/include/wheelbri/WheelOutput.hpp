#pragma once

// What the service sends to the physical wheel's force feedback, in the numbers each system's API takes.
// Kept free of <dinput.h> and <linux/input.h> (plain numbers) so it is unit-tested on every platform; the
// service copies them into DIEFFECT / struct ff_effect.
//
// The numbers are exactly the ones SDL3 (3.4) computed when the service played effects through SDL's
// haptic API, so the wheel gets the same commands as before:
//   FfbEffect --toWheelEffect--> WheelEffect (SDL_HapticEffect's units) --> DirectInput or evdev units.

#include "wheelbri/Ffb.hpp"

#include <cstdint>

namespace wheelbri {

// One effect for the wheel's steering axis, in SDL_HapticEffect's units: levels and coefficients
// -32767..32767, saturations and dead band 0..65535, period in ms, phase in hundredths of a degree.
struct WheelEffect {
    FfbEffectType type = FfbEffectType::Constant;
    std::int16_t level = 0;            // constant force

    std::int16_t rightCoefficient = 0; // conditions (spring, damper, friction, inertia)
    std::int16_t leftCoefficient = 0;
    std::uint16_t rightSaturation = 0;
    std::uint16_t leftSaturation = 0;
    std::uint16_t deadband = 0;
    std::int16_t center = 0;

    std::uint16_t periodMs = 0;        // periodic (sine, square, triangle, sawtooth)
    std::int16_t magnitude = 0;
    std::int16_t offset = 0;
    std::uint16_t phase = 0;

    friend bool operator==(const WheelEffect&, const WheelEffect&) = default;
};

// `invert` mirrors the effect (the wheel's direction is reversed): forces and centres change sign, a
// condition's left and right sides swap.
[[nodiscard]] WheelEffect toWheelEffect(const FfbEffect& effect, bool invert);

// --- DirectInput (Windows) --------------------------------------------------------------------------

namespace di {

// Which DIEFFECT type-specific block the effect uses.
enum class Block { Constant, Condition, Periodic };

// DICONSTANTFORCE / DICONDITION (one axis) / DIPERIODIC fields; units -10000..10000 / 0..10000.
struct Output {
    Block block = Block::Constant;
    std::int32_t magnitude = 0;            // DICONSTANTFORCE::lMagnitude

    std::int32_t offset = 0;               // DICONDITION
    std::int32_t positiveCoefficient = 0;
    std::int32_t negativeCoefficient = 0;
    std::uint32_t positiveSaturation = 0;
    std::uint32_t negativeSaturation = 0;
    std::int32_t deadBand = 0;

    std::uint32_t periodicMagnitude = 0;   // DIPERIODIC
    std::int32_t periodicOffset = 0;
    std::uint32_t phase = 0;               // hundredths of a degree
    std::uint32_t periodUs = 0;

    friend bool operator==(const Output&, const Output&) = default;
};

// SDL_SYS_ToDIEFFECT's conversions. The DIEFFECT around it: one axis (the first force-feedback axis),
// DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS, direction 0, gain 10000, no envelope, no trigger.
[[nodiscard]] Output toDirectInput(const WheelEffect& effect);

}  // namespace di

// --- evdev (Linux) ----------------------------------------------------------------------------------

namespace evdev {

// <linux/input.h> effect types and waveforms (checked against the header where it is included).
inline constexpr std::uint16_t kFfPeriodic = 0x51;
inline constexpr std::uint16_t kFfConstant = 0x52;
inline constexpr std::uint16_t kFfSpring = 0x53;
inline constexpr std::uint16_t kFfFriction = 0x54;
inline constexpr std::uint16_t kFfDamper = 0x55;
inline constexpr std::uint16_t kFfInertia = 0x56;
inline constexpr std::uint16_t kFfSquare = 0x58;
inline constexpr std::uint16_t kFfTriangle = 0x59;
inline constexpr std::uint16_t kFfSine = 0x5a;
inline constexpr std::uint16_t kFfSawUp = 0x5b;
inline constexpr std::uint16_t kFfSawDown = 0x5c;
inline constexpr std::uint16_t kFfGain = 0x60;
inline constexpr std::uint16_t kFfAutocenter = 0x61;

// struct ff_effect's fields for one effect on the steering axis (direction 0x4000, played until stopped).
struct Output {
    std::uint16_t type = kFfConstant;
    std::uint16_t direction = 0x4000;
    std::int16_t level = 0;                // ff_constant_effect

    std::uint16_t rightSaturation = 0;     // ff_condition_effect, first axis (the second stays zero)
    std::uint16_t leftSaturation = 0;
    std::int16_t rightCoefficient = 0;
    std::int16_t leftCoefficient = 0;
    std::uint16_t deadband = 0;
    std::int16_t center = 0;

    std::uint16_t waveform = 0;            // ff_periodic_effect
    std::uint16_t periodMs = 0;
    std::int16_t magnitude = 0;
    std::int16_t offset = 0;
    std::uint16_t phase = 0;               // 0..0xFFFF for 0..360 degrees

    friend bool operator==(const Output&, const Output&) = default;
};

// SDL_SYS_ToFFEffect's conversions.
[[nodiscard]] Output toEvdev(const WheelEffect& effect);

}  // namespace evdev

}  // namespace wheelbri
