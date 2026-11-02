#include "wheelbri/LiveMapping.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

EmulatorConfig running() {
    EmulatorConfig config;
    config.mapper.throttle.sourceAxis = 1;
    config.mapper.accessories[0] = DeviceId{0x346E, 0x1000};
    return config;
}

TEST(LiveMapping, NothingChangedIsNothing) {
    const LiveMappingChange change = liveMappingChange(running(), running());
    EXPECT_FALSE(change.any());
    EXPECT_FALSE(change.rotationChanged);
}

TEST(LiveMapping, TheMappingIsUsedAtOnce) {
    EmulatorConfig next = running();
    next.mapper.throttle.invert = true;
    next.mapper.buttonMap[3] = 12;
    const LiveMappingChange change = liveMappingChange(running(), next);
    ASSERT_TRUE(change.mapper.has_value());
    EXPECT_EQ(*change.mapper, next.mapper);
    EXPECT_FALSE(change.rotationChanged);
    EXPECT_TRUE(change.waiting.empty());
}

TEST(LiveMapping, TheRotationIsUsedAtOnceAndSaid) {
    EmulatorConfig next = running();
    next.mapper.steering.targetRotationDeg = 540.0F;
    const LiveMappingChange change = liveMappingChange(running(), next);
    ASSERT_TRUE(change.mapper.has_value());
    EXPECT_FLOAT_EQ(change.mapper->steering.targetRotationDeg, 540.0F);
    EXPECT_TRUE(change.rotationChanged);
}

// The session opened the accessories at Start: a mapping that reads another one waits, whole.
TEST(LiveMapping, OtherAccessoriesWaitForTheNextStart) {
    EmulatorConfig next = running();
    next.mapper.accessories[1] = DeviceId{0x044F, 0xB696};
    next.mapper.throttle.invert = true;
    const LiveMappingChange change = liveMappingChange(running(), next);
    EXPECT_FALSE(change.mapper.has_value());
    ASSERT_EQ(change.waiting.size(), 1U);
    EXPECT_NE(change.waiting[0].find("accessories"), std::string::npos);
}

TEST(LiveMapping, ButtonOrderIsUsedAtOnce) {
    EmulatorConfig next = running();
    next.buttonOrder = ButtonOrder::Native;
    const LiveMappingChange change = liveMappingChange(running(), next);
    ASSERT_TRUE(change.buttonOrder.has_value());
    EXPECT_EQ(*change.buttonOrder, ButtonOrder::Native);
    EXPECT_FALSE(change.mapper.has_value());
}

TEST(LiveMapping, StartOnlyValuesWaitAndAreNamed) {
    EmulatorConfig next = running();
    next.targetProfile = "g25";
    next.statePort = 41331;
    next.ffbInvert = true;
    next.centringSpringPercent = 40.0F;
    const LiveMappingChange change = liveMappingChange(running(), next);
    EXPECT_FALSE(change.mapper.has_value());
    ASSERT_EQ(change.waiting.size(), 3U);
    EXPECT_EQ(change.waiting[0], "the target");
    EXPECT_EQ(change.waiting[1], "the ports");
    EXPECT_EQ(change.waiting[2], "[ffb]");
}

// Logging, telemetry, retention and the per-game sections are not this path's.
TEST(LiveMapping, OtherSectionsAreNotCompared) {
    EmulatorConfig next = running();
    next.telemetry.enabled = !next.telemetry.enabled;
    next.gameRotations["nfspayback"] = GameRotationOverride{};
    EXPECT_FALSE(liveMappingChange(running(), next).any());
}

TEST(LiveMapping, AppliedChangeIsTheNewBaseline) {
    EmulatorConfig now = running();
    EmulatorConfig next = running();
    next.mapper.steering.targetRotationDeg = 270.0F;
    next.buttonOrder = ButtonOrder::Native;
    applyLiveMappingChange(now, liveMappingChange(now, next));
    EXPECT_EQ(now.mapper, next.mapper);
    EXPECT_EQ(now.buttonOrder, ButtonOrder::Native);
    EXPECT_FALSE(liveMappingChange(now, next).any());
}

}  // namespace
}  // namespace wheelbri
