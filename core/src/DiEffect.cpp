// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/DiEffect.hpp"

#include <algorithm>
#include <cmath>

namespace wheelbri::di {

namespace {

constexpr float kFull = 10000.0F;
constexpr double kPi = 3.14159265358979323846;

// A value a little over full scale is clamped; one far outside it is garbage and gives no force (see
// isGarbage in the header).
float signedUnit(const std::int32_t value) {
    if (isGarbage(value)) return 0.0F;
    return std::clamp(static_cast<float>(value) / kFull, -1.0F, 1.0F);
}
float unsignedUnit(const std::uint32_t value) {
    if (isGarbage(value)) return 0.0F;
    return std::clamp(static_cast<float>(value) / kFull, 0.0F, 1.0F);
}

}  // namespace

float directionAlongX(const std::uint32_t coordinates, const std::int32_t firstDirection) {
    if ((coordinates & kCartesian) != 0) {
        return firstDirection < 0 ? -1.0F : 1.0F;
    }
    const double radians = static_cast<double>(firstDirection) / 100.0 * kPi / 180.0;
    if ((coordinates & kPolar) != 0) {
        return static_cast<float>(std::sin(radians));   // polar: 0 = north, 9000 = east (+X)
    }
    if ((coordinates & kSpherical) != 0) {
        return static_cast<float>(std::cos(radians));   // spherical: first angle measured from +X
    }
    return 1.0F;
}

FfbEffect constantForce(const std::int32_t magnitude, const float direction, const std::uint32_t gain) {
    FfbEffect effect;
    effect.type = FfbEffectType::Constant;
    effect.constantLevel = std::clamp(signedUnit(magnitude) * direction, -1.0F, 1.0F);
    effect.gain = unsignedUnit(gain);
    return effect;
}

FfbEffect condition(const FfbEffectType type, const std::int32_t offset, const std::int32_t positiveCoefficient,
                    const std::int32_t negativeCoefficient, const std::uint32_t positiveSaturation,
                    const std::uint32_t negativeSaturation, const std::int32_t deadBand, const std::uint32_t gain) {
    FfbEffect effect;
    effect.type = type;
    effect.condition.center = signedUnit(offset);
    effect.condition.deadband = std::clamp(static_cast<float>(deadBand) / kFull, 0.0F, 1.0F);
    effect.condition.positiveCoefficient = signedUnit(positiveCoefficient);
    effect.condition.negativeCoefficient = signedUnit(negativeCoefficient);
    // DirectInput meaning kept as is: saturation 0 = the condition produces no force on that side.
    effect.condition.positiveSaturation = unsignedUnit(positiveSaturation);
    effect.condition.negativeSaturation = unsignedUnit(negativeSaturation);
    effect.gain = unsignedUnit(gain);
    return effect;
}

FfbEffect periodic(const FfbEffectType type, const std::uint32_t magnitude, const std::int32_t offset,
                   const std::uint32_t phase, const std::uint32_t periodUs, const float direction,
                   const std::uint32_t gain) {
    FfbEffect effect;
    effect.type = type;
    const float along = std::clamp(direction, -1.0F, 1.0F);
    effect.periodic.magnitude = unsignedUnit(magnitude) * std::abs(along);
    effect.periodic.offset = signedUnit(offset) * along;
    effect.periodic.periodMs = static_cast<float>(periodUs) / 1000.0F;
    float phaseDeg = static_cast<float>(phase % 36000U) / 100.0F;
    if (along < 0.0F) {
        phaseDeg = std::fmod(phaseDeg + 180.0F, 360.0F);
    }
    effect.periodic.phaseDeg = phaseDeg;
    effect.gain = unsignedUnit(gain);
    return effect;
}

FfbEnvelope envelope(const std::uint32_t attackLevel, const std::uint32_t attackTimeUs, const std::uint32_t fadeLevel,
                     const std::uint32_t fadeTimeUs) {
    FfbEnvelope out;
    out.attackLevel = unsignedUnit(attackLevel);
    out.attackMs = static_cast<float>(attackTimeUs) / 1000.0F;
    out.fadeLevel = unsignedUnit(fadeLevel);
    out.fadeMs = static_cast<float>(fadeTimeUs) / 1000.0F;
    return out;
}

}  // namespace wheelbri::di
