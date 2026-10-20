#include "wheelbri/CentringSpring.hpp"

#include <algorithm>
#include <cmath>

namespace wheelbri {

FfbEffect centringSpringEffect(const float percent) {
    const float p = std::isfinite(percent) ? std::clamp(percent, 0.0F, kCentringSpringMaxPercent) : 0.0F;
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.center = 0.0F;
    spring.condition.deadband = 0.0F;
    spring.condition.positiveCoefficient = p / kCentringSpringMaxPercent;
    spring.condition.negativeCoefficient = p / kCentringSpringMaxPercent;
    spring.condition.positiveSaturation = 1.0F;
    spring.condition.negativeSaturation = 1.0F;
    spring.gain = 1.0F;
    return spring;
}

}  // namespace wheelbri
