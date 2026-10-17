#include "wheelbri/Ffb.hpp"

#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <cmath>

namespace wheelbri {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.ffb");
    return logger;
}

}  // namespace

const char* ffbTypeName(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return "constant";
        case FfbEffectType::Spring: return "spring";
        case FfbEffectType::Damper: return "damper";
        case FfbEffectType::Friction: return "friction";
        case FfbEffectType::Inertia: return "inertia";
        case FfbEffectType::Sine: return "sine";
        case FfbEffectType::Square: return "square";
        case FfbEffectType::Triangle: return "triangle";
        case FfbEffectType::SawtoothUp: return "saw up";
        case FfbEffectType::SawtoothDown: return "saw down";
    }
    return "?";
}

bool isConditionEffect(const FfbEffectType type) {
    return type == FfbEffectType::Spring || type == FfbEffectType::Damper || type == FfbEffectType::Friction ||
           type == FfbEffectType::Inertia;
}

float ffbStrength(const FfbEffect& e) {
    if (e.type == FfbEffectType::Constant) {
        return e.constantLevel * e.gain;
    }
    if (isConditionEffect(e.type)) {
        return std::max(std::abs(e.condition.positiveCoefficient), std::abs(e.condition.negativeCoefficient)) *
               e.gain;
    }
    return e.periodic.magnitude * e.gain;
}

namespace {

// A force from a game (or from a bug) that is not a number becomes no force.
float finiteOr(const float value, const float fallback) { return std::isfinite(value) ? value : fallback; }

}  // namespace

FfbSafetyLimiter::FfbSafetyLimiter(const FfbSafetyConfig& config) : config_(config) {
    // NaN would pass std::clamp unchanged and switch the cap off: fall back to the safe defaults.
    const FfbSafetyConfig defaults;
    if (!std::isfinite(config_.masterGain) || !std::isfinite(config_.maxMagnitude) ||
        !std::isfinite(config_.maxSlewPerSecond)) {
        WE_LOG_ERROR(log(), "ffb limiter: config is not a number (gain={} max={} slew={}): using the defaults "
                            "gain={:.2f} max={:.2f} slew={:.1f}/s",
                     config_.masterGain, config_.maxMagnitude, config_.maxSlewPerSecond, defaults.masterGain,
                     defaults.maxMagnitude, defaults.maxSlewPerSecond);
        config_.masterGain = finiteOr(config_.masterGain, defaults.masterGain);
        config_.maxMagnitude = finiteOr(config_.maxMagnitude, defaults.maxMagnitude);
        config_.maxSlewPerSecond = finiteOr(config_.maxSlewPerSecond, defaults.maxSlewPerSecond);
    }
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

FfbEffect FfbSafetyLimiter::apply(const FfbEffect& input, const float dtSeconds) {
    // Every field finite and in its range before anything else (packets come from other processes).
    FfbEffect requested = input;
    requested.gain = std::clamp(finiteOr(requested.gain, 0.0F), 0.0F, 1.0F);
    requested.constantLevel = finiteOr(requested.constantLevel, 0.0F);
    FfbCondition& c = requested.condition;
    c.center = std::clamp(finiteOr(c.center, 0.0F), -1.0F, 1.0F);
    c.deadband = std::clamp(finiteOr(c.deadband, 0.0F), 0.0F, 1.0F);
    c.positiveCoefficient = finiteOr(c.positiveCoefficient, 0.0F);
    c.negativeCoefficient = finiteOr(c.negativeCoefficient, 0.0F);
    c.positiveSaturation = finiteOr(c.positiveSaturation, 0.0F);
    c.negativeSaturation = finiteOr(c.negativeSaturation, 0.0F);
    FfbPeriodic& p = requested.periodic;
    p.magnitude = finiteOr(p.magnitude, 0.0F);
    p.offset = finiteOr(p.offset, 0.0F);
    p.periodMs = std::clamp(finiteOr(p.periodMs, 100.0F), 1.0F, 65535.0F);
    p.phaseDeg = finiteOr(p.phaseDeg, 0.0F);
    if (!std::isfinite(lastConstant_)) lastConstant_ = 0.0F;
    const float dt = std::clamp(finiteOr(dtSeconds, 0.0F), 0.0F, 1.0F);

    FfbEffect limited = requested;
    limited.gain = 1.0F;  // folded into the magnitudes below
    switch (requested.type) {
        case FfbEffectType::Constant:
            limited.constantLevel = limitConstant(requested.constantLevel * requested.gain, dt);
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
        default: {
            // A type this build does not know (a newer or broken sender): no force, never its numbers unlimited.
            const FfbEffectType type = requested.type;
            limited = FfbEffect{};
            limited.type = type;
            limited.condition.positiveSaturation = 0.0F;
            limited.condition.negativeSaturation = 0.0F;
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
    // A dead band over the whole travel: the condition can never push, so it is sent as an empty one. Not as asked:
    // the R3 drove itself to full lock and held there with NFS Unbound's idle spring (coefficient 1, saturation 0,
    // dead band 100%; 2026-10-11).
    if (!(std::fabs(requested.deadband) < kWholeTravel)) {
        FfbCondition none;
        none.positiveSaturation = 0.0F;
        none.negativeSaturation = 0.0F;
        return none;
    }
    const float gain = config_.masterGain;
    const float cap = config_.maxMagnitude;
    // A saturation of 0 is "no limit" to the R3 (NFS Heat's spring had 0 and still pushed): the cap then.
    const auto saturation = [&](const float s) { return s > 0.0F ? std::clamp(s * gain, 0.0F, cap) : cap; };
    FfbCondition limited = requested;
    limited.positiveCoefficient = std::clamp(requested.positiveCoefficient * gain, -1.0F, 1.0F);
    limited.negativeCoefficient = std::clamp(requested.negativeCoefficient * gain, -1.0F, 1.0F);
    limited.positiveSaturation = saturation(requested.positiveSaturation);
    limited.negativeSaturation = saturation(requested.negativeSaturation);
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

}  // namespace wheelbri
