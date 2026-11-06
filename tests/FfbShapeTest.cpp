// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/DiEffect.hpp"
#include "wheelbri/FfbShape.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

FfbEffect constant(const float level) {
    FfbEffect e;
    e.type = FfbEffectType::Constant;
    e.constantLevel = level;
    return e;
}

FfbEnvelope envelope(const float attackLevel, const float attackMs, const float fadeLevel, const float fadeMs) {
    FfbEnvelope env;
    env.attackLevel = attackLevel;
    env.attackMs = attackMs;
    env.fadeLevel = fadeLevel;
    env.fadeMs = fadeMs;
    return env;
}

TEST(FfbShape, NoShapeLeavesTheEffectAlone) {
    const FfbEffect e = constant(0.6F);
    EXPECT_EQ(shapeAt(e, FfbShape{}, 123.0, 1000.0), e);
}

TEST(FfbShape, AttackRisesThenHoldsThenFades) {
    FfbShape shape;
    shape.envelope = envelope(0.0F, 200.0F, 0.0F, 400.0F);
    const FfbEffect e = constant(0.8F);
    EXPECT_NEAR(shapeAt(e, shape, 0.0, 1000.0).constantLevel, 0.0F, 1e-5F);
    EXPECT_NEAR(shapeAt(e, shape, 100.0, 1000.0).constantLevel, 0.4F, 1e-5F);
    EXPECT_NEAR(shapeAt(e, shape, 400.0, 1000.0).constantLevel, 0.8F, 1e-5F);   // sustain: unchanged
    EXPECT_NEAR(shapeAt(e, shape, 800.0, 1000.0).constantLevel, 0.4F, 1e-5F);   // half way through the fade
    EXPECT_NEAR(shapeAt(e, shape, 1000.0, 1000.0).constantLevel, 0.0F, 1e-5F);
}

TEST(FfbShape, ALeftForceKeepsItsSign) {
    FfbShape shape;
    shape.envelope = envelope(0.2F, 100.0F, 0.0F, 0.0F);
    EXPECT_NEAR(shapeAt(constant(-0.6F), shape, 50.0, kUntilStopped).constantLevel, -0.4F, 1e-5F);
}

TEST(FfbShape, AnAttackAboveTheLevelIsAKickThatSettles) {
    FfbShape shape;
    shape.envelope = envelope(1.0F, 100.0F, 0.0F, 0.0F);
    EXPECT_NEAR(shapeAt(constant(0.2F), shape, 0.0, kUntilStopped).constantLevel, 1.0F, 1e-5F);
    EXPECT_NEAR(shapeAt(constant(0.2F), shape, 100.0, kUntilStopped).constantLevel, 0.2F, 1e-5F);
}

TEST(FfbShape, NoDurationMeansNoFade) {
    FfbShape shape;
    shape.envelope = envelope(0.8F, 0.0F, 0.0F, 500.0F);
    EXPECT_NEAR(shapeAt(constant(0.8F), shape, 1e9, kUntilStopped).constantLevel, 0.8F, 1e-5F);
}

TEST(FfbShape, OverlappingAttackAndFadeEndAtTheFadeLevel) {
    FfbShape shape;
    shape.envelope = envelope(0.0F, 300.0F, 0.1F, 300.0F);
    EXPECT_NEAR(shapeAt(constant(1.0F), shape, 200.0, 200.0).constantLevel, 0.1F, 1e-5F);
}

TEST(FfbShape, PeriodicMagnitudeFollowsTheEnvelopeAndConditionsDoNot) {
    FfbShape shape;
    shape.envelope = envelope(0.0F, 100.0F, 0.0F, 0.0F);
    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = 0.5F;
    sine.periodic.offset = 0.2F;
    const FfbEffect half = shapeAt(sine, shape, 50.0, kUntilStopped);
    EXPECT_NEAR(half.periodic.magnitude, 0.25F, 1e-5F);
    EXPECT_NEAR(half.periodic.offset, 0.2F, 1e-5F);   // the offset is not enveloped

    FfbEffect spring;
    spring.type = FfbEffectType::Spring;
    spring.condition.positiveCoefficient = 0.7F;
    EXPECT_EQ(shapeAt(spring, shape, 50.0, kUntilStopped), spring);
}

TEST(FfbShape, RampGoesFromStartToEndOverTheDuration) {
    FfbShape shape;
    shape.rampEndLevel = 0.5F;
    const FfbEffect ramp = constant(-0.5F);
    EXPECT_NEAR(shapeAt(ramp, shape, 0.0, 2000.0).constantLevel, -0.5F, 1e-5F);
    EXPECT_NEAR(shapeAt(ramp, shape, 500.0, 2000.0).constantLevel, -0.25F, 1e-5F);
    EXPECT_NEAR(shapeAt(ramp, shape, 2000.0, 2000.0).constantLevel, 0.5F, 1e-5F);
    EXPECT_NEAR(shapeAt(ramp, shape, 500.0, kUntilStopped).constantLevel, 0.0F, 1e-5F);   // infinite: the mean
}

TEST(FfbShape, LevelsStayInRange) {
    FfbShape shape;
    shape.envelope = envelope(2.0F, 100.0F, -1.0F, 100.0F);   // nonsense from a game
    for (double t = 0; t <= 1000.0; t += 50.0) {
        const float level = shapeAt(constant(0.9F), shape, t, 1000.0).constantLevel;
        EXPECT_GE(level, 0.0F);
        EXPECT_LE(level, 1.0F);
    }
}

TEST(DiEffect, EnvelopeUnits) {
    const FfbEnvelope env = di::envelope(2500, 150'000, 10000, 2'000'000);
    EXPECT_NEAR(env.attackLevel, 0.25F, 1e-5F);
    EXPECT_NEAR(env.attackMs, 150.0F, 1e-3F);
    EXPECT_NEAR(env.fadeLevel, 1.0F, 1e-5F);
    EXPECT_NEAR(env.fadeMs, 2000.0F, 1e-3F);
    EXPECT_NEAR(di::envelope(20000, 0, 0, 0).attackLevel, 1.0F, 1e-5F);   // clamped
}

// An effect with a known duration and an attack but no fade (DIENVELOPE with dwFadeTime 0): it holds its level to
// the end.
TEST(FfbShape, AKnownDurationWithoutFadeHoldsToTheEnd) {
    FfbShape shape;
    shape.envelope = envelope(0.0F, 100.0F, 0.0F, 0.0F);
    EXPECT_NEAR(shapeAt(constant(0.6F), shape, 50.0, 1000.0).constantLevel, 0.3F, 1e-5F);
    EXPECT_NEAR(shapeAt(constant(0.6F), shape, 999.0, 1000.0).constantLevel, 0.6F, 1e-5F);
}

// A ramp with a duration of 0 (no time to go anywhere) plays at its mean level, as one without a duration does.
TEST(FfbShape, ARampWithNoDurationPlaysItsMean) {
    FfbShape shape;
    shape.rampEndLevel = 0.8F;
    EXPECT_NEAR(shapeAt(constant(0.2F), shape, 100.0, 0.0).constantLevel, 0.5F, 1e-5F);
    EXPECT_NEAR(shapeAt(constant(0.2F), shape, -5.0, 1000.0).constantLevel, 0.2F, 1e-5F);   // before the start
}

// No condition (spring, damper, friction, inertia) has an envelope or a ramp: each comes back as it was.
TEST(FfbShape, NoConditionTypeIsShaped) {
    FfbShape shape;
    shape.envelope = envelope(0.0F, 100.0F, 0.0F, 100.0F);
    shape.rampEndLevel = 1.0F;
    for (const FfbEffectType type :
         {FfbEffectType::Spring, FfbEffectType::Damper, FfbEffectType::Friction, FfbEffectType::Inertia}) {
        FfbEffect condition;
        condition.type = type;
        condition.condition.positiveCoefficient = 0.7F;
        EXPECT_EQ(shapeAt(condition, shape, 50.0, 1000.0), condition);
    }
    FfbEffect sine;   // a periodic effect with only a ramp (which it ignores): unchanged too
    sine.type = FfbEffectType::Sine;
    sine.periodic.magnitude = 0.4F;
    FfbShape rampOnly;
    rampOnly.rampEndLevel = 1.0F;
    EXPECT_EQ(shapeAt(sine, rampOnly, 50.0, 1000.0), sine);
}

// A constant level above full scale (a sender that did not clamp) comes out of the envelope at full scale at most.
TEST(FfbShape, AnEnvelopeNeverGoesPastFullScale) {
    FfbShape shape;
    shape.envelope = envelope(0.5F, 100.0F, 0.0F, 0.0F);
    EXPECT_NEAR(shapeAt(constant(-3.0F), shape, 500.0, kUntilStopped).constantLevel, -1.0F, 1e-5F);
}

}  // namespace
}  // namespace wheelbri
