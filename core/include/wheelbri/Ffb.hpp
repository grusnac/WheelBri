#pragma once

#include <cstdint>

namespace wheelbri {

// Common force feedback model. Every source (DirectInput effects, Logitech SDK calls,
// Logitech HID commands) is translated into this, and every sink plays it.
enum class FfbEffectType : std::uint8_t {
    Constant,
    Spring,
    Damper,
    Friction,
    Inertia,
    Sine,
    Square,
    Triangle,
    SawtoothUp,
    SawtoothDown,
};

// Condition effects (spring, damper, friction, inertia). Coefficients in [-1, 1], saturations in [0, 1].
struct FfbCondition {
    float center = 0.0F;
    float deadband = 0.0F;
    float positiveCoefficient = 0.0F;
    float negativeCoefficient = 0.0F;
    float positiveSaturation = 1.0F;
    float negativeSaturation = 1.0F;

    friend bool operator==(const FfbCondition&, const FfbCondition&) = default;
};

struct FfbPeriodic {
    float magnitude = 0.0F;             // 0..1
    float offset = 0.0F;                // -1..1
    float periodMs = 0.0F;
    float phaseDeg = 0.0F;

    friend bool operator==(const FfbPeriodic&, const FfbPeriodic&) = default;
};

struct FfbEffect {
    FfbEffectType type = FfbEffectType::Constant;
    float constantLevel = 0.0F;         // -1 (full left) .. 1 (full right)
    FfbCondition condition;
    FfbPeriodic periodic;
    float gain = 1.0F;                  // per-effect gain, 0..1

    friend bool operator==(const FfbEffect&, const FfbEffect&) = default;
};

// Short lower-case name ("constant", "spring", "saw up", ...), for displays, logs and telemetry.
[[nodiscard]] const char* ffbTypeName(FfbEffectType type);

// Spring, damper, friction and inertia: forces that depend on the wheel's position or motion.
[[nodiscard]] bool isConditionEffect(FfbEffectType type);

// One number per effect, gain folded in the way the limiter does it: the signed force of a constant,
// the larger coefficient of a condition, the magnitude of a periodic effect.
[[nodiscard]] float ffbStrength(const FfbEffect& effect);

struct FfbSafetyConfig {
    float masterGain = 1.0F;            // 0..1; the strength itself is set in the wheel's own app (FfbLimits.hpp)
    float maxMagnitude = 1.0F;          // hard cap, 0..1: 1 = all the game asks; the wheel's app sets the strength
    float maxSlewPerSecond = 50.0F;     // constant force may move at most this much full-scale per second
    // A constant push the game leaves exactly as it is this long (seconds) fades out, with the periodic effects on
    // top: a game that paused (loading screen) but keeps sending its last force. 0 = never (FfbController).
    float frozenForceSeconds = 2.0F;
    friend bool operator==(const FfbSafetyConfig&, const FfbSafetyConfig&) = default;
};

// Last stage before the physical wheel: applies the user gain and protects against jolts.
// Stateful (slew limit on the constant force), so use one limiter per played effect.
class FfbSafetyLimiter {
public:
    explicit FfbSafetyLimiter(const FfbSafetyConfig& config);

    [[nodiscard]] FfbEffect apply(const FfbEffect& requested, float dtSeconds);
    void reset() { lastConstant_ = 0.0F; }
    // Other limits from now on (a game's own, FfbController::setGameTuning), checked as the constructor does; the
    // constant force slews on from where it is.
    void setConfig(const FfbSafetyConfig& config);
    [[nodiscard]] const FfbSafetyConfig& config() const { return config_; }

private:
    [[nodiscard]] float limitConstant(float requested, float dtSeconds);
    [[nodiscard]] FfbCondition limitCondition(const FfbCondition& requested) const;
    static constexpr float kWholeTravel = 0.999F;   // a condition's dead band from here on covers all of it
    [[nodiscard]] FfbPeriodic limitPeriodic(const FfbPeriodic& requested) const;

    FfbSafetyConfig config_;
    float lastConstant_ = 0.0F;
};

}  // namespace wheelbri
