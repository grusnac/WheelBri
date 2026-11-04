#include "wheelbri/WheelOutput.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wheelbri {

namespace {

// Non-finite values (a NaN from a broken packet) become 0: no force.
float finite(const float value) { return std::isfinite(value) ? value : 0.0F; }

std::int16_t toSigned(float value) {
    value = finite(value);
    return static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

std::uint16_t toUnsigned(float value) {
    value = finite(value);
    return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F));
}

}  // namespace

WheelEffect toWheelEffect(const FfbEffect& effect, const bool invert) {
    const float sign = invert ? -1.0F : 1.0F;
    WheelEffect out;
    out.type = effect.type;
    switch (effect.type) {
        case FfbEffectType::Constant:
            out.level = toSigned(sign * effect.constantLevel);
            break;

        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia: {
            const FfbCondition& c = effect.condition;
            // Inverting mirrors the effect: left and right swap, the centre flips.
            out.rightCoefficient = toSigned(invert ? c.negativeCoefficient : c.positiveCoefficient);
            out.leftCoefficient = toSigned(invert ? c.positiveCoefficient : c.negativeCoefficient);
            out.rightSaturation = toUnsigned(invert ? c.negativeSaturation : c.positiveSaturation);
            out.leftSaturation = toUnsigned(invert ? c.positiveSaturation : c.negativeSaturation);
            out.deadband = toUnsigned(c.deadband);
            out.center = toSigned(sign * c.center);
            break;
        }

        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown: {
            const FfbPeriodic& p = effect.periodic;
            // Clamped as a float first: lround of a huge value is undefined (it gave a 1 ms buzz).
            out.periodMs = static_cast<std::uint16_t>(std::lround(std::clamp(finite(p.periodMs), 1.0F, 65535.0F)));
            out.magnitude = toSigned(p.magnitude);
            out.offset = toSigned(sign * p.offset);
            // Inverted, the whole wave turns: its offset, and its swing half a turn on (as DiEffect and HidPid turn
            // an effect whose direction points the other way); only the offset turning left the swing as it was.
            float phase = std::fmod(finite(p.phaseDeg) + (invert ? 180.0F : 0.0F), 360.0F);
            if (phase < 0.0F) {
                phase += 360.0F;
            }
            out.phase = static_cast<std::uint16_t>(std::lround(phase * 100.0F) % 36000);
            break;
        }
    }
    return out;
}

namespace di {

namespace {

// SDL_dinputhaptic.c: CONVERT and CCONVERT (integer arithmetic, truncating like C).
std::int32_t convert(const std::int32_t x) { return x * 10000 / 0x7FFF; }
std::uint32_t clampConvert(const std::int32_t x) {
    return x > 0x7FFF ? 10000U : static_cast<std::uint32_t>(x * 10000 / 0x7FFF);
}

}  // namespace

Output toDirectInput(const WheelEffect& e) {
    Output out;
    switch (e.type) {
        case FfbEffectType::Constant:
            out.block = Block::Constant;
            out.magnitude = convert(e.level);
            break;
        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia:
            out.block = Block::Condition;
            out.offset = convert(e.center);
            out.positiveCoefficient = convert(e.rightCoefficient);
            out.negativeCoefficient = convert(e.leftCoefficient);
            out.positiveSaturation = clampConvert(e.rightSaturation / 2);
            out.negativeSaturation = clampConvert(e.leftSaturation / 2);
            out.deadBand = static_cast<std::int32_t>(clampConvert(e.deadband / 2));
            break;
        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown:
            out.block = Block::Periodic;
            out.periodicMagnitude = static_cast<std::uint32_t>(convert(std::abs(static_cast<std::int32_t>(e.magnitude))));
            out.periodicOffset = convert(e.offset);
            out.phase = (static_cast<std::uint32_t>(e.phase) + (e.magnitude < 0 ? 18000U : 0U)) % 36000U;
            out.periodUs = static_cast<std::uint32_t>(e.periodMs) * 1000U;
            break;
    }
    return out;
}

}  // namespace di

namespace evdev {

namespace {

// SDL_syshaptic.c (Linux): CLAMP.
std::uint16_t clampLength(const std::uint16_t x) { return x > 32767 ? 32767 : x; }

}  // namespace

Output toEvdev(const WheelEffect& e) {
    Output out;
    switch (e.type) {
        case FfbEffectType::Constant:
            out.type = kFfConstant;
            out.level = e.level;
            break;
        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia:
            out.type = e.type == FfbEffectType::Spring   ? kFfSpring
                       : e.type == FfbEffectType::Damper ? kFfDamper
                       : e.type == FfbEffectType::Friction ? kFfFriction
                                                           : kFfInertia;
            out.rightSaturation = e.rightSaturation;
            out.leftSaturation = e.leftSaturation;
            out.rightCoefficient = e.rightCoefficient;
            out.leftCoefficient = e.leftCoefficient;
            out.deadband = e.deadband;
            out.center = e.center;
            break;
        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown:
            out.type = kFfPeriodic;
            out.waveform = e.type == FfbEffectType::Sine       ? kFfSine
                           : e.type == FfbEffectType::Square   ? kFfSquare
                           : e.type == FfbEffectType::Triangle ? kFfTriangle
                           : e.type == FfbEffectType::SawtoothUp ? kFfSawUp
                                                                 : kFfSawDown;
            out.periodMs = clampLength(e.periodMs);
            out.magnitude = e.magnitude;
            out.offset = e.offset;
            out.phase = static_cast<std::uint16_t>(static_cast<std::uint32_t>(e.phase) * 0x10000U / 36000U);
            break;
    }
    return out;
}

}  // namespace evdev

}  // namespace wheelbri
