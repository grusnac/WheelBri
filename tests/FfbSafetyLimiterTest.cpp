#include "wheelbri/Ffb.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace wheelbri {
namespace {

constexpr float kTolerance = 1e-5F;

FfbEffect constant(const float level) {
    FfbEffect effect;
    effect.type = FfbEffectType::Constant;
    effect.constantLevel = level;
    return effect;
}

TEST(FfbSafetyLimiter, CapsConstantMagnitude) {
    FfbSafetyLimiter limiter({1.0F, 0.5F, 1000.0F});
    EXPECT_NEAR(limiter.apply(constant(1.0F), 1.0F).constantLevel, 0.5F, kTolerance);
    EXPECT_NEAR(limiter.apply(constant(-1.0F), 1.0F).constantLevel, -0.5F, kTolerance);
}

TEST(FfbSafetyLimiter, AppliesMasterAndEffectGain) {
    FfbSafetyLimiter limiter({0.5F, 1.0F, 1000.0F});
    FfbEffect effect = constant(0.8F);
    effect.gain = 0.5F;
    EXPECT_NEAR(limiter.apply(effect, 1.0F).constantLevel, 0.2F, kTolerance);
}

TEST(FfbSafetyLimiter, SlewLimitsSuddenJumps) {
    FfbSafetyLimiter limiter({1.0F, 1.0F, 10.0F});
    // 1 ms at 10 full-scale/s allows a step of 0.01.
    EXPECT_NEAR(limiter.apply(constant(1.0F), 0.001F).constantLevel, 0.01F, kTolerance);
    EXPECT_NEAR(limiter.apply(constant(1.0F), 0.001F).constantLevel, 0.02F, kTolerance);
    EXPECT_NEAR(limiter.apply(constant(-1.0F), 0.001F).constantLevel, 0.01F, kTolerance);
}

TEST(FfbSafetyLimiter, ResetClearsSlewHistory) {
    FfbSafetyLimiter limiter({1.0F, 1.0F, 1.0F});
    EXPECT_NEAR(limiter.apply(constant(1.0F), 0.5F).constantLevel, 0.5F, kTolerance);
    limiter.reset();
    // Ramps from zero again instead of continuing from 0.5.
    EXPECT_NEAR(limiter.apply(constant(1.0F), 0.1F).constantLevel, 0.1F, kTolerance);
}

TEST(FfbSafetyLimiter, CapsConditionSaturation) {
    FfbSafetyLimiter limiter({1.0F, 0.6F, 1000.0F});
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 1.0F;
    spring.condition.negativeCoefficient = 1.0F;
    const FfbEffect limited = limiter.apply(spring, 0.001F);
    EXPECT_NEAR(limited.condition.positiveSaturation, 0.6F, kTolerance);
    EXPECT_NEAR(limited.condition.negativeSaturation, 0.6F, kTolerance);
    EXPECT_NEAR(limited.condition.positiveCoefficient, 1.0F, kTolerance);
}

// NFS Unbound's idle spring (2026-10-11): coefficient 1, saturation 0, dead band over the whole travel. It can never
// push, and the R3 drove itself to full lock with it: sent as an empty condition.
TEST(FfbSafetyLimiter, AConditionThatCanNeverPushIsSentEmpty) {
    FfbSafetyLimiter limiter({0.7F, 0.8F, 50.0F});
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 1.0F;
    spring.condition.negativeCoefficient = 1.0F;
    spring.condition.positiveSaturation = 0.0F;
    spring.condition.negativeSaturation = 0.0F;
    spring.condition.deadband = 1.0F;
    const FfbEffect limited = limiter.apply(spring, 0.001F);
    EXPECT_EQ(limited.condition.positiveCoefficient, 0.0F);
    EXPECT_EQ(limited.condition.negativeCoefficient, 0.0F);
    EXPECT_EQ(limited.condition.positiveSaturation, 0.0F);
    EXPECT_EQ(limited.condition.deadband, 0.0F);
}

// A saturation of 0 is "no limit" to the R3 (NFS Heat's spring): it gets the cap, never more.
TEST(FfbSafetyLimiter, AZeroSaturationGetsTheCap) {
    FfbSafetyLimiter limiter({0.7F, 0.8F, 50.0F});
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.15F;
    spring.condition.negativeCoefficient = 0.15F;
    spring.condition.positiveSaturation = 0.0F;
    spring.condition.negativeSaturation = 0.5F;
    const FfbEffect limited = limiter.apply(spring, 0.001F);
    EXPECT_NEAR(limited.condition.positiveSaturation, 0.8F, kTolerance);
    EXPECT_NEAR(limited.condition.negativeSaturation, 0.35F, kTolerance);
    EXPECT_NEAR(limited.condition.positiveCoefficient, 0.105F, kTolerance);
}

TEST(FfbSafetyLimiter, PeriodicMagnitudePlusOffsetStaysUnderCap) {
    FfbSafetyLimiter limiter({1.0F, 0.8F, 1000.0F});
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = 0.6F;
    sine.periodic.offset = 0.5F;
    const FfbEffect limited = limiter.apply(sine, 0.001F);
    EXPECT_NEAR(limited.periodic.magnitude, 0.6F, kTolerance);
    EXPECT_NEAR(limited.periodic.offset, 0.2F, kTolerance);
}

TEST(FfbSafetyLimiter, NotANumberConfigFallsBackToTheDefaults) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    FfbSafetyLimiter limiter({1.0F, nan, nan});
    // The default cap (0.8) and slew limit hold: a full request is capped, and it rises gradually.
    const float first = limiter.apply(constant(1.0F), 0.001F).constantLevel;
    EXPECT_GT(first, 0.0F);
    EXPECT_LT(first, 0.1F);
    float level = first;
    for (int i = 0; i < 2000; ++i) level = limiter.apply(constant(1.0F), 0.001F).constantLevel;
    EXPECT_NEAR(level, FfbSafetyConfig{}.maxMagnitude, kTolerance);
}

TEST(FfbSafetyLimiter, NotANumberEffectBecomesNoForce) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    FfbSafetyLimiter limiter({1.0F, 0.8F, 1000.0F});
    EXPECT_NEAR(limiter.apply(constant(nan), 0.01F).constantLevel, 0.0F, kTolerance);
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = inf;
    sine.periodic.offset = nan;
    sine.periodic.periodMs = 1e30F;
    const FfbEffect out = limiter.apply(sine, 0.01F);
    EXPECT_LE(out.periodic.magnitude, 0.8F);
    EXPECT_TRUE(std::isfinite(out.periodic.offset));
    EXPECT_LE(out.periodic.periodMs, 65535.0F);
    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.gain = nan;
    spring.condition.positiveCoefficient = 1.0F;
    EXPECT_NEAR(limiter.apply(spring, 0.01F).condition.positiveCoefficient, 0.0F, kTolerance);
}

TEST(FfbSafetyLimiter, InvalidConfigIsClamped) {
    FfbSafetyLimiter limiter({5.0F, 2.0F, -1.0F});
    // Gain clamps to 1, cap to 1, slew to 0: output cannot move from zero.
    EXPECT_NEAR(limiter.apply(constant(1.0F), 1.0F).constantLevel, 0.0F, kTolerance);
}

}  // namespace
}  // namespace wheelbri
