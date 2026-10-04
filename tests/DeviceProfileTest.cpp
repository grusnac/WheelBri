#include "wheelbri/DeviceProfile.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace wheelbri {
namespace {

TEST(DeviceProfile, SteeringSpansTheFullRange) {
    const DeviceProfile profile(g920Spec());
    WheelState state;
    state.steering = -1.0F;
    EXPECT_EQ(profile.encode(state).steering, 0);
    state.steering = 1.0F;
    EXPECT_EQ(profile.encode(state).steering, 65535);
    state.steering = 0.0F;
    EXPECT_EQ(profile.encode(state).steering, 32768);
}

TEST(DeviceProfile, PedalsReleasedAtMaxAreInverted) {
    const DeviceProfile profile(g920Spec());
    WheelState state;
    state.throttle = 0.0F;
    state.brake = 1.0F;
    const TargetReport report = profile.encode(state);
    EXPECT_EQ(report.throttle, 65535);
    EXPECT_EQ(report.brake, 0);
}

TEST(DeviceProfile, OutOfRangeValuesAreClamped) {
    DeviceProfileSpec spec = g920Spec();
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
    const DeviceProfile profile(g920Spec());
    WheelState state;
    state.buttons = 0xA5;
    state.hat = 6;
    const TargetReport report = profile.encode(state);
    EXPECT_EQ(report.buttons, 0xA5U);
    EXPECT_EQ(report.hat, 6);
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
    EXPECT_TRUE(profile.buttonName(11).empty());       // joy.cpl 12: there, but nothing presses it
    EXPECT_EQ(profile.buttonName(12), "Gear 1");       // joy.cpl 13, Logitech's shifter FAQ
    EXPECT_EQ(profile.buttonName(17), "Gear 6");
    EXPECT_EQ(profile.buttonName(18), "Gear R");       // joy.cpl 19
    EXPECT_TRUE(profile.buttonName(19).empty());
    EXPECT_EQ(profile.buttonCount(), kG920Buttons);
    EXPECT_EQ(kG920Buttons, 19U);
}

TEST(DeviceProfile, OnlyTheG920sOwnButtonsReachTheGame) {
    const DeviceProfile profile(g920Spec());
    WheelState state;
    state.buttons = 0xFFFFFFFFU;
    const std::uint32_t buttons = profile.encode(state).buttons;
    EXPECT_EQ(buttons, 0x7F7FFU);            // 0-10 and 12-18
    EXPECT_EQ(buttons & (1U << 11), 0U);     // the unused 12th
    EXPECT_EQ(buttons >> 19, 0U);            // nothing above the shifter
}

TEST(DeviceProfile, ValuesThatAreNotNumbersReadAsNeutral) {
    const DeviceProfile profile(g920Spec());
    WheelState state;
    state.steering = std::numeric_limits<float>::quiet_NaN();
    state.throttle = std::numeric_limits<float>::infinity();
    state.brake = std::numeric_limits<float>::quiet_NaN();
    const TargetReport report = profile.encode(state);
    EXPECT_EQ(report.steering, 32768);   // centred, not full left lock
    EXPECT_EQ(report.throttle, 65535);   // released (the G920 reports released at the top)
    EXPECT_EQ(report.brake, 65535);
}

TEST(DeviceProfile, FactoryKnowsBuiltInProfiles) {
    const auto g920 = makeProfile("g920");
    ASSERT_NE(g920, nullptr);
    EXPECT_EQ(g920->identity().productId, 0xC262);
    EXPECT_EQ(makeProfile("g29"), nullptr);   // the G920 is the one target
    EXPECT_EQ(makeProfile("t300"), nullptr);
}

}  // namespace
}  // namespace wheelbri
