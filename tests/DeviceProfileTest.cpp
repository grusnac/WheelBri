#include "wheelbri/DeviceProfile.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

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
    const auto g25 = makeProfile("g25");
    ASSERT_NE(g25, nullptr);
    EXPECT_EQ(g25->identity().productId, 0xC299);
    EXPECT_EQ(makeProfile("g29"), nullptr);   // the G920 and the G25 are the targets
    EXPECT_EQ(makeProfile("t300"), nullptr);
}

TEST(DeviceProfile, G25IsTheNativeModeWheel) {
    const DeviceProfile profile(g25Spec());
    EXPECT_EQ(profile.identity().shortName, "G25");
    EXPECT_EQ(profile.identity().vendorId, 0x046D);
    EXPECT_EQ(profile.identity().productId, 0xC299);   // native mode, never the Driving Force mode's C294
    EXPECT_EQ(profile.identity().productName, "Logitech G25 Racing Wheel USB");
    EXPECT_FLOAT_EQ(profile.identity().rotationDeg, 900.0F);
    EXPECT_EQ(toString(profile.identity().directInputProductGuid()), "{C299046D-0000-0000-0000-504944564944}");
}

TEST(DeviceProfile, G25ButtonsHaveTheGearsAtLogitechsNumbers) {
    const DeviceProfile profile(g25Spec());
    EXPECT_EQ(profile.buttonCount(), kG25Buttons);
    EXPECT_EQ(kG25Buttons, 19U);
    EXPECT_EQ(profile.buttonName(kG25FirstGear), "Gear 1");      // joy.cpl 9
    EXPECT_EQ(profile.buttonName(kG25FirstGear + 5), "Gear 6");
    EXPECT_EQ(profile.buttonName(kG25FirstGear + 6), "Gear R");  // joy.cpl 15
    for (std::size_t i = 0; i < kG25Buttons; ++i) EXPECT_FALSE(profile.buttonName(i).empty()) << i;
    EXPECT_TRUE(profile.buttonName(kG25Buttons).empty());
}

TEST(DeviceProfile, G25EncodesLikeTheG920) {
    const DeviceProfile g25(g25Spec());
    const DeviceProfile g920(g920Spec());
    WheelState state;
    state.steering = -0.25F;
    state.throttle = 0.5F;
    state.brake = 1.0F;
    state.clutch = 0.0F;
    state.hat = 3;
    state.buttons = 0xFFFFFFFFU;
    const TargetReport report = g25.encode(state);
    EXPECT_EQ(report.steering, g920.encode(state).steering);
    EXPECT_EQ(report.throttle, g920.encode(state).throttle);
    EXPECT_EQ(report.brake, 0);          // fully pressed: the bottom of the range
    EXPECT_EQ(report.clutch, 65535);     // released: the top
    EXPECT_EQ(report.hat, 3);
    EXPECT_EQ(report.buttons, (1U << kG25Buttons) - 1U);   // all 19, nothing above
}

// The Mapping tab's button groups: every named button once, related ones together.
TEST(ButtonLayout, EveryNamedButtonOnceInGroups) {
    for (const DeviceProfileSpec& spec : {g920Spec(), g25Spec(), g27Spec()}) {
        const DeviceProfile profile(spec);
        const ButtonLayout layout = layoutButtons(profile);
        std::vector<int> seen(profile.buttonCount(), 0);
        for (const ButtonCell& cell : layout.cells) {
            if (cell.heading) {
                EXPECT_NE(cell.text, "OTHER") << spec.identity.shortName;   // the groups name them all
                continue;
            }
            ASSERT_LT(cell.button, seen.size());
            ++seen[cell.button];
        }
        for (std::size_t b = 0; b < seen.size(); ++b) {
            EXPECT_EQ(seen[b], profile.buttonName(b).empty() ? 0 : 1) << spec.identity.shortName << " " << b;
        }
    }
}

TEST(ButtonLayout, G920FaceButtonsInOneColumnAndTheShifterBelow) {
    const DeviceProfile profile(g920Spec());
    const ButtonLayout layout = layoutButtons(profile);
    const auto find = [&](const std::size_t b) {
        for (const ButtonCell& cell : layout.cells) {
            if (!cell.heading && cell.button == b) return cell;
        }
        return ButtonCell{-1, 0.0F, false, {}, 0};
    };
    // A, B, X, Y: the left column, one under the other, under FACE.
    ASSERT_EQ(layout.cells.front(), (ButtonCell{0, 0.0F, true, "FACE", 0}));
    for (std::size_t b = 0; b < 4; ++b) {
        EXPECT_EQ(find(b).column, 0);
        EXPECT_FLOAT_EQ(find(b).y, kButtonHeadingH + static_cast<float>(b) * kButtonRowH);
    }
    EXPECT_EQ(find(9).column, 1);   // LSB, RSB: right
    EXPECT_EQ(find(8).column, 1);
    // The gears below both columns, two a row: Gear 1 left, Gear 2 right on one row.
    const ButtonCell gear1 = find(kG920FirstGear);
    const ButtonCell gear2 = find(kG920FirstGear + 1);
    EXPECT_EQ(gear1.column, 0);
    EXPECT_EQ(gear2.column, 1);
    EXPECT_FLOAT_EQ(gear1.y, gear2.y);
    EXPECT_GT(gear1.y, find(5).y);   // below both paddles, the left column's last
    EXPECT_GT(gear1.y, find(4).y);
    EXPECT_FLOAT_EQ(layout.height, find(kG920FirstGear + 6).y + kButtonRowH);
}

// A profile without groups: its named buttons in number order, two a row, no heading.
TEST(ButtonLayout, NoGroupsIsNumberOrder) {
    DeviceProfileSpec spec = g920Spec();
    spec.buttonGroups.clear();
    const DeviceProfile profile(spec);
    const ButtonLayout layout = layoutButtons(profile);
    ASSERT_FALSE(layout.cells.empty());
    EXPECT_FALSE(layout.cells.front().heading);
    EXPECT_EQ(layout.cells[0].button, 0U);
    EXPECT_EQ(layout.cells[1].button, 1U);
    EXPECT_EQ(layout.cells[1].column, 1);
}

}  // namespace
}  // namespace wheelbri
