#include "wheelemu/Ffb.hpp"

#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <cmath>

namespace wheelemu {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.ffb");
    return logger;
}

}  // namespace

FfbSafetyLimiter::FfbSafetyLimiter(const FfbSafetyConfig& config) : config_(config) {
    config_.masterGain = std::clamp(config_.masterGain, 0.0F, 1.0F);
    config_.maxMagnitude = std::clamp(config_.maxMagnitude, 0.0F, 1.0F);
    config_.maxSlewPerSecond = std::max(config_.maxSlewPerSecond, 0.0F);
    if (config_.masterGain != config.masterGain || config_.maxMagnitude != config.maxMagnitude ||
        config_.maxSlewPerSecond != config.maxSlewPerSecond) {
        WE_LOG_WARN(log(), "ffb limiter: config out of range, clamped to gain={:.2f} max={:.2f} slew={:.1f}/s",
                    config_.masterGain, config_.maxMagnitude, config_.maxSlewPerSecond);
    }
    WE_LOG_DEBUG(log(), "ffb limiter: gain={:.2f} max_magnitude={:.2f} max_slew={:.1f}/s", config_.masterGain,
                 config_.maxMagnitude, config_.maxSlewPerSecond);
}

FfbEffect FfbSafetyLimiter::apply(const FfbEffect& requested, const float dtSeconds) {
    FfbEffect limited = requested;
    limited.gain = 1.0F;  // folded into the magnitudes below
    switch (requested.type) {
        case FfbEffectType::Constant:
            limited.constantLevel = limitConstant(requested.constantLevel * requested.gain, dtSeconds);
            break;
        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia: {
            FfbCondition scaled = requested.condition;
            scaled.positiveCoefficient *= requested.gain;
            scaled.negativeCoefficient *= requested.gain;
            limited.condition = limitCondition(scaled);
            break;
        }
        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown: {
            FfbPeriodic scaled = requested.periodic;
            scaled.magnitude *= requested.gain;
            scaled.offset *= requested.gain;
            limited.periodic = limitPeriodic(scaled);
            break;
        }
    }
    return limited;
}

float FfbSafetyLimiter::limitConstant(const float requested, const float dtSeconds) {
    const float scaled = requested * config_.masterGain;
    const float target = std::clamp(scaled, -config_.maxMagnitude, config_.maxMagnitude);
    const float maxStep = config_.maxSlewPerSecond * std::max(dtSeconds, 0.0F);
    lastConstant_ = std::clamp(target, lastConstant_ - maxStep, lastConstant_ + maxStep);

    // Clamping is expected now and then; constant clamping means the gain is set too high for this game.
    if (std::abs(scaled) > config_.maxMagnitude) {
        static logging::RateLimiter limiter(std::chrono::seconds(10));
        WE_LOG_WARN_LIMITED(log(), limiter, "ffb limiter: constant force {:+.3f} capped at {:.3f}, consider a lower gain",
                            scaled, config_.maxMagnitude);
    }
    return lastConstant_;
}

FfbCondition FfbSafetyLimiter::limitCondition(const FfbCondition& requested) const {
    const float gain = config_.masterGain;
    const float cap = config_.maxMagnitude;
    FfbCondition limited = requested;
    limited.positiveCoefficient = std::clamp(requested.positiveCoefficient * gain, -1.0F, 1.0F);
    limited.negativeCoefficient = std::clamp(requested.negativeCoefficient * gain, -1.0F, 1.0F);
    limited.positiveSaturation = std::clamp(requested.positiveSaturation * gain, 0.0F, cap);
    limited.negativeSaturation = std::clamp(requested.negativeSaturation * gain, 0.0F, cap);
    return limited;
}

FfbPeriodic FfbSafetyLimiter::limitPeriodic(const FfbPeriodic& requested) const {
    FfbPeriodic limited = requested;
    const float gain = config_.masterGain;
    const float cap = config_.maxMagnitude;
    limited.magnitude = std::clamp(requested.magnitude * gain, 0.0F, cap);
    // Offset and magnitude together must not exceed the cap.
    const float offsetCap = std::max(cap - limited.magnitude, 0.0F);
    limited.offset = std::clamp(requested.offset * gain, -offsetCap, offsetCap);
    return limited;
}

}  // namespace wheelemu
