// SPDX-License-Identifier: GPL-3.0-or-later
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

// Every type name, for the window, the log and telemetry; a type this build does not know shows as "?".
TEST(FfbEffectType, EveryTypeHasAShortName) {
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Constant), "constant");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Spring), "spring");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Damper), "damper");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Friction), "friction");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Inertia), "inertia");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Sine), "sine");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Square), "square");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::Triangle), "triangle");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::SawtoothUp), "saw up");
    EXPECT_STREQ(ffbTypeName(FfbEffectType::SawtoothDown), "saw down");
    EXPECT_STREQ(ffbTypeName(static_cast<FfbEffectType>(77)), "?");
}

TEST(FfbEffectType, ConditionsAreTheFourThatResist) {
    EXPECT_TRUE(isConditionEffect(FfbEffectType::Spring));
    EXPECT_TRUE(isConditionEffect(FfbEffectType::Damper));
    EXPECT_TRUE(isConditionEffect(FfbEffectType::Friction));
    EXPECT_TRUE(isConditionEffect(FfbEffectType::Inertia));
    EXPECT_FALSE(isConditionEffect(FfbEffectType::Constant));
    EXPECT_FALSE(isConditionEffect(FfbEffectType::Sine));
    EXPECT_FALSE(isConditionEffect(FfbEffectType::SawtoothDown));
}

// The strength the window shows: signed for a constant, the larger coefficient of a condition, the magnitude of a
// periodic effect, each times its own gain.
TEST(FfbEffectType, StrengthFoldsInTheEffectGain) {
    FfbEffect push = constant(-0.5F);
    push.gain = 0.5F;
    EXPECT_FLOAT_EQ(ffbStrength(push), -0.25F);
    FfbEffect friction;
    friction.type = FfbEffectType::Friction;
    friction.condition.positiveCoefficient = 0.2F;
    friction.condition.negativeCoefficient = -0.6F;
    EXPECT_FLOAT_EQ(ffbStrength(friction), 0.6F);
    FfbEffect saw;
    saw.type = FfbEffectType::SawtoothUp;
    saw.periodic.magnitude = 0.4F;
    saw.gain = 0.5F;
    EXPECT_FLOAT_EQ(ffbStrength(saw), 0.2F);
}

// Each config value that is not a number falls back to its own default; the others stay as given.
TEST(FfbSafetyLimiter, EachNotANumberConfigValueFallsBackOnItsOwn) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    FfbSafetyLimiter capNan({0.5F, nan, 1000.0F});   // the default cap 1, the given gain 0.5
    EXPECT_NEAR(capNan.apply(constant(1.0F), 1.0F).constantLevel, 0.5F, kTolerance);
    EXPECT_NEAR(capNan.apply(constant(4.0F), 1.0F).constantLevel, 1.0F, kTolerance);
    FfbSafetyLimiter slewInf({1.0F, 0.6F, inf});      // an infinite slew is no slew limit: the default 50/s instead
    EXPECT_NEAR(slewInf.apply(constant(1.0F), 0.001F).constantLevel, 0.05F, kTolerance);
    FfbSafetyLimiter gainNan({nan, 0.6F, 1000.0F});   // the default gain 1
    EXPECT_NEAR(gainNan.apply(constant(0.3F), 1.0F).constantLevel, 0.3F, kTolerance);
}

// A negative slew limit (a typo in the config) is clamped to 0 on its own: the constant force cannot move at all.
TEST(FfbSafetyLimiter, ANegativeSlewLimitHoldsTheForceAtZero) {
    FfbSafetyLimiter limiter({1.0F, 0.8F, -5.0F});
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(limiter.apply(constant(1.0F), 0.1F).constantLevel, 0.0F);
    }
}

// The step is clamped too: a non-finite, negative or huge time step from a stalled loop never lets the force jump
// further than one second's worth of slew.
TEST(FfbSafetyLimiter, OddTimeStepsDoNotBypassTheSlewLimit) {
    FfbSafetyLimiter limiter({1.0F, 1.0F, 0.5F});
    EXPECT_EQ(limiter.apply(constant(1.0F), std::numeric_limits<float>::quiet_NaN()).constantLevel, 0.0F);
    EXPECT_EQ(limiter.apply(constant(1.0F), -3.0F).constantLevel, 0.0F);
    EXPECT_NEAR(limiter.apply(constant(1.0F), 1e9F).constantLevel, 0.5F, kTolerance);
    EXPECT_NEAR(limiter.apply(constant(1.0F), std::numeric_limits<float>::infinity()).constantLevel, 0.5F,
                kTolerance);   // inf is not a number of seconds: no step
}

// Every condition type is capped the same way, and coefficients past the full range are clamped to it.
TEST(FfbSafetyLimiter, EveryConditionTypeIsCapped) {
    for (const FfbEffectType type :
         {FfbEffectType::Spring, FfbEffectType::Damper, FfbEffectType::Friction, FfbEffectType::Inertia}) {
        FfbSafetyLimiter limiter({1.0F, 0.6F, 50.0F});
        FfbEffect condition;
        condition.type = type;
        condition.condition.positiveCoefficient = 3.0F;
        condition.condition.negativeCoefficient = -3.0F;
        condition.condition.positiveSaturation = 1.0F;
        condition.condition.negativeSaturation = -1.0F;   // negative: as "no limit", so the cap
        condition.condition.center = 5.0F;
        condition.condition.deadband = -1.0F;
        const FfbEffect limited = limiter.apply(condition, 0.001F);
        EXPECT_EQ(limited.type, type);
        EXPECT_FLOAT_EQ(limited.condition.positiveCoefficient, 1.0F);
        EXPECT_FLOAT_EQ(limited.condition.negativeCoefficient, -1.0F);
        EXPECT_NEAR(limited.condition.positiveSaturation, 0.6F, kTolerance);
        EXPECT_NEAR(limited.condition.negativeSaturation, 0.6F, kTolerance);
        EXPECT_FLOAT_EQ(limited.condition.center, 1.0F);
        EXPECT_FLOAT_EQ(limited.condition.deadband, 0.0F);
        EXPECT_FLOAT_EQ(limited.gain, 1.0F);
    }
}

// Every periodic type is capped the same way; a negative magnitude (never meant: magnitudes are 0..1) is no wave, and
// the offset alone still stays under the cap. Phase and period are kept (the period within its range). A whole second
// between the two, so the slew has arrived.
TEST(FfbSafetyLimiter, EveryPeriodicTypeIsCapped) {
    for (const FfbEffectType type : {FfbEffectType::Sine, FfbEffectType::Square, FfbEffectType::Triangle,
                                     FfbEffectType::SawtoothUp, FfbEffectType::SawtoothDown}) {
        FfbSafetyLimiter limiter({1.0F, 0.5F, 50.0F});
        FfbEffect wave;
        wave.type = type;
        wave.periodic = {-0.9F, -2.0F, 0.0F, 450.0F};
        const FfbEffect limited = limiter.apply(wave, 1.0F);
        EXPECT_EQ(limited.type, type);
        EXPECT_FLOAT_EQ(limited.periodic.magnitude, 0.0F);
        EXPECT_NEAR(limited.periodic.offset, -0.5F, kTolerance);
        EXPECT_FLOAT_EQ(limited.periodic.periodMs, 1.0F);
        EXPECT_FLOAT_EQ(limited.periodic.phaseDeg, 450.0F);
        wave.periodic = {3.0F, 1.0F, 1e6F, -90.0F};
        const FfbEffect strong = limiter.apply(wave, 1.0F);
        EXPECT_NEAR(strong.periodic.magnitude, 0.5F, kTolerance);
        EXPECT_NEAR(strong.periodic.offset, 0.0F, kTolerance);   // the magnitude took the whole cap
        EXPECT_FLOAT_EQ(strong.periodic.periodMs, 65535.0F);
    }
}

// The per-effect gain is clamped to 0..1: a gain above 1 cannot make an effect stronger than asked.
TEST(FfbSafetyLimiter, EffectGainAboveOneIsOne) {
    FfbSafetyLimiter limiter({1.0F, 1.0F, 1000.0F});
    FfbEffect push = constant(0.3F);
    push.gain = 4.0F;
    EXPECT_NEAR(limiter.apply(push, 1.0F).constantLevel, 0.3F, kTolerance);
    push.gain = -1.0F;
    EXPECT_NEAR(limiter.apply(push, 1.0F).constantLevel, 0.0F, kTolerance);
}

// An effect type this build does not know (a newer or broken sender) must not pass its numbers to the wheel
// unlimited: it leaves the limiter with no force at all.
TEST(FfbSafetyLimiter, AnUnknownEffectTypeIsNoForce) {
    FfbSafetyLimiter limiter({1.0F, 0.5F, 50.0F});
    FfbEffect unknown;
    unknown.type = static_cast<FfbEffectType>(42);
    unknown.constantLevel = 5.0F;
    unknown.condition = {0.0F, 0.0F, 4.0F, 4.0F, 3.0F, 3.0F};
    unknown.periodic = {6.0F, 6.0F, 50.0F, 0.0F};
    const FfbEffect limited = limiter.apply(unknown, 1.0F);
    EXPECT_EQ(limited.constantLevel, 0.0F);
    EXPECT_EQ(limited.condition.positiveCoefficient, 0.0F);
    EXPECT_EQ(limited.condition.negativeCoefficient, 0.0F);
    EXPECT_EQ(limited.condition.positiveSaturation, 0.0F);
    EXPECT_EQ(limited.condition.negativeSaturation, 0.0F);
    EXPECT_EQ(limited.periodic.magnitude, 0.0F);
    EXPECT_EQ(limited.periodic.offset, 0.0F);
}

// A periodic effect's offset and magnitude slew as a constant force does: a square wave with a full offset is a push,
// and does not reach the wheel at once.
TEST(FfbSafetyLimiter, PeriodicOffsetAndMagnitudeSlew) {
    FfbSafetyLimiter limiter({1.0F, 1.0F, 10.0F});
    FfbEffect square;
    square.type = FfbEffectType::Square;
    square.periodic = {0.0F, 1.0F, 65535.0F, 0.0F};
    EXPECT_NEAR(limiter.apply(square, 0.001F).periodic.offset, 0.01F, kTolerance);
    EXPECT_NEAR(limiter.apply(square, 0.001F).periodic.offset, 0.02F, kTolerance);
    square.periodic = {0.5F, 0.0F, 40.0F, 0.0F};
    const FfbEffect next = limiter.apply(square, 0.001F);
    EXPECT_NEAR(next.periodic.magnitude, 0.01F, kTolerance);
    EXPECT_NEAR(next.periodic.offset, 0.01F, kTolerance);
    limiter.reset();
    square.periodic = {0.0F, -1.0F, 40.0F, 0.0F};
    EXPECT_NEAR(limiter.apply(square, 0.001F).periodic.offset, -0.01F, kTolerance);   // from none again
}

// While magnitude and offset slew toward a new split of the cap, their sum never goes over it.
TEST(FfbSafetyLimiter, PeriodicSlewKeepsTheSumUnderTheCap) {
    FfbSafetyLimiter limiter({1.0F, 0.5F, 50.0F});
    FfbEffect wave;
    wave.type = FfbEffectType::Sine;
    wave.periodic = {0.0F, 0.5F, 40.0F, 0.0F};
    (void)limiter.apply(wave, 1.0F);
    wave.periodic = {0.5F, -0.5F, 40.0F, 0.0F};   // all to the wave, the offset to the other side
    for (int i = 0; i < 50; ++i) {
        const FfbEffect limited = limiter.apply(wave, 0.004F);
        EXPECT_LE(limited.periodic.magnitude + std::abs(limited.periodic.offset), 0.5F + kTolerance) << i;
    }
    // A lower cap from now on (a game's own): the sum is held to it at once, not slewed down.
    limiter.setConfig({1.0F, 0.2F, 50.0F});
    const FfbEffect lowered = limiter.apply(wave, 0.004F);
    EXPECT_LE(lowered.periodic.magnitude + std::abs(lowered.periodic.offset), 0.2F + kTolerance);
}

}  // namespace
}  // namespace wheelbri
