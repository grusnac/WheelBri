// SPDX-License-Identifier: GPL-3.0-or-later
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
    float masterGain = 1.0F;            // 0..1; 1 in use: the strength itself is set in the wheel's own app
    float maxMagnitude = 1.0F;          // hard cap, 0..1: 1 in use, all the game asks ([ffb] max_magnitude retired)
    // A constant force, and a periodic effect's magnitude and offset, may move at most this much full-scale per
    // second.
    float maxSlewPerSecond = 50.0F;
    // A constant push the game leaves exactly as it is this long (seconds) fades out, with the periodic effects on
    // top: a game that paused (loading screen) but keeps sending its last force. 0 = never (FfbController).
    float frozenForceSeconds = 2.0F;
    // A constant force at exactly full scale while the game has sent no force in between for 2 s plays as none
    // (FfbController, "lone full push"): Race Driver: GRID's broken value, which Windows' force feedback driver turns
    // into a full push before WheelBri sees it (Mode Virtual). false = every full push plays.
    bool ignoreLoneFullPush = true;
    friend bool operator==(const FfbSafetyConfig&, const FfbSafetyConfig&) = default;
};

// Last stage before the physical wheel: applies the user gain and protects against jolts.
// Stateful (slew limit on the constant force and on a periodic effect's magnitude and offset: a square wave with
// offset 1 is a push like a constant one), so use one limiter per played effect.
class FfbSafetyLimiter {
public:
    explicit FfbSafetyLimiter(const FfbSafetyConfig& config);

    [[nodiscard]] FfbEffect apply(const FfbEffect& requested, float dtSeconds);
    void reset() {
        lastConstant_ = 0.0F;
        lastMagnitude_ = 0.0F;
        lastOffset_ = 0.0F;
    }
    // Other limits from now on (a game's own, FfbController::setGameTuning), checked as the constructor does; the
    // forces slew on from where they are.
    void setConfig(const FfbSafetyConfig& config);
    [[nodiscard]] const FfbSafetyConfig& config() const { return config_; }

private:
    [[nodiscard]] float limitConstant(float requested, float dtSeconds);
    [[nodiscard]] FfbCondition limitCondition(const FfbCondition& requested) const;
    static constexpr float kWholeTravel = 0.999F;   // a condition's dead band from here on covers all of it
    [[nodiscard]] FfbPeriodic limitPeriodic(const FfbPeriodic& requested, float dtSeconds);
    // `value` moved from `last` toward `target` by at most the slew step for `dtSeconds`; `last` becomes the result.
    [[nodiscard]] float slewed(float& last, float target, float dtSeconds) const;

    FfbSafetyConfig config_;
    float lastConstant_ = 0.0F;
    float lastMagnitude_ = 0.0F;
    float lastOffset_ = 0.0F;
};

}  // namespace wheelbri
