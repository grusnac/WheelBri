#include "wheelemu/DeviceProfile.hpp"

#include <gtest/gtest.h>

namespace wheelemu {
namespace {

TEST(DeviceIdentity, DirectInputProductGuidForG29) {
    const DeviceProfile profile(g29Spec());
    EXPECT_EQ(toString(profile.identity().directInputProductGuid()), "{C24F046D-0000-0000-0000-504944564944}");
}

TEST(DeviceProfile, SteeringSpansTheFullRange) {
    const DeviceProfile profile(g29Spec());
    WheelState state;
    state.steering = -1.0F;
    EXPECT_EQ(profile.encode(state).steering, 0);
    state.steering = 1.0F;
    EXPECT_EQ(profile.encode(state).steering, 65535);
    state.steering = 0.0F;
    EXPECT_EQ(profile.encode(state).steering, 32768);
}

TEST(DeviceProfile, PedalsReleasedAtMaxAreInverted) {
    const DeviceProfile profile(g29Spec());
    WheelState state;
    state.throttle = 0.0F;
    state.brake = 1.0F;
    const TargetReport report = profile.encode(state);
    EXPECT_EQ(report.throttle, 65535);
    EXPECT_EQ(report.brake, 0);
}

TEST(DeviceProfile, OutOfRangeValuesAreClamped) {
    DeviceProfileSpec spec = g29Spec();
    spec.pedalsReleasedAtMax = false;
    const DeviceProfile profile(spec);
    WheelState state;
    state.steering = 3.0F;
    state.clutch = -2.0F;
    const TargetReport report = profile.encode(state);
    EXPECT_EQ(report.steering, 65535);
    EXPECT_EQ(report.clutch, 0);
}

TEST(DeviceProfile, ButtonsAndHatPassThrough) {
    const DeviceProfile profile(g29Spec());
    WheelState state;
    state.buttons = 0xA5;
    state.hat = 6;
    const TargetReport report = profile.encode(state);
    EXPECT_EQ(report.buttons, 0xA5U);
    EXPECT_EQ(report.hat, 6);
}

TEST(DeviceProfile, G29ButtonNames) {
    const DeviceProfile profile(g29Spec());
    EXPECT_EQ(profile.buttonName(0), "Cross");
    EXPECT_EQ(profile.buttonName(12), "Gear 1");
    EXPECT_EQ(profile.buttonName(24), "PS");
    EXPECT_TRUE(profile.buttonName(25).empty());   // the G29 has 25 buttons
    EXPECT_TRUE(profile.buttonName(1000).empty());
}

TEST(DeviceIdentity, DirectInputProductGuidForG920) {
    const DeviceProfile profile(g920Spec());
    EXPECT_EQ(toString(profile.identity().directInputProductGuid()), "{C262046D-0000-0000-0000-504944564944}");
    EXPECT_EQ(profile.identity().shortName, "G920");
}

TEST(DeviceProfile, G920ButtonNames) {
    const DeviceProfile profile(g920Spec());
    EXPECT_EQ(profile.buttonName(0), "A");
    EXPECT_EQ(profile.buttonName(10), "Xbox");
    EXPECT_TRUE(profile.buttonName(11).empty());
}

TEST(DeviceProfile, FactoryKnowsBuiltInProfiles) {
    const auto g920 = makeProfile("g920");
    ASSERT_NE(g920, nullptr);
    EXPECT_EQ(g920->identity().productId, 0xC262);
    const auto g29 = makeProfile("g29");
    ASSERT_NE(g29, nullptr);
    EXPECT_EQ(g29->identity().productId, 0xC24F);
    EXPECT_EQ(makeProfile("t300"), nullptr);
}

}  // namespace
}  // namespace wheelemu
