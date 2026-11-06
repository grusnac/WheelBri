// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/Mapper.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace wheelbri {
namespace {

constexpr float kTolerance = 1e-5F;

MapperConfig pedalConfig() {
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    config.brake.sourceAxis = 2;
    config.clutch.sourceAxis = 3;
    return config;
}

RawInputState withAxis(const std::size_t axis, const float value) {
    RawInputState raw;
    raw.axes[axis] = value;
    return raw;
}

TEST(MapperSteering, SameRotationPassesThrough) {
    const Mapper mapper(MapperConfig{});
    EXPECT_NEAR(mapper.map(withAxis(0, 0.5F)).steering, 0.5F, kTolerance);
    EXPECT_NEAR(mapper.map(withAxis(0, -1.0F)).steering, -1.0F, kTolerance);
}

// The steering rotation is the game's full lock and what the wheel's own software is set to: the axis goes through 1:1
// whatever it is (no separate wheel rotation since 2026-10-13), so the wheel's end stop is the game's full lock.
TEST(MapperSteering, OneToOneWhateverTheRotation) {
    for (const float rotation : {180.0F, 540.0F, 900.0F, 1080.0F}) {
        MapperConfig config;
        config.steering.targetRotationDeg = rotation;
        const Mapper mapper(config);
        EXPECT_NEAR(mapper.map(withAxis(0, 1.0F)).steering, 1.0F, kTolerance) << rotation;
        EXPECT_NEAR(mapper.map(withAxis(0, 0.25F)).steering, 0.25F, kTolerance) << rotation;
        EXPECT_NEAR(mapper.map(withAxis(0, -0.5F)).steering, -0.5F, kTolerance) << rotation;
    }
}

TEST(MapperSteering, NoDeadZoneAroundTheCentre) {
    const Mapper mapper{MapperConfig{}};
    EXPECT_NEAR(mapper.map(withAxis(0, 0.01F)).steering, 0.01F, kTolerance);   // the smallest move counts
    EXPECT_NEAR(mapper.map(withAxis(0, -0.01F)).steering, -0.01F, kTolerance);
    EXPECT_EQ(mapper.map(withAxis(0, 0.0F)).steering, 0.0F);
    EXPECT_NEAR(mapper.map(withAxis(0, -1.0F)).steering, -1.0F, kTolerance);
}

TEST(MapperSteering, InvertFlipsDirection) {
    MapperConfig config;
    config.steering.invert = true;
    const Mapper mapper(config);
    EXPECT_NEAR(mapper.map(withAxis(0, 0.25F)).steering, -0.25F, kTolerance);
}

TEST(MapperPedals, BipolarAxisBecomesTravel) {
    const Mapper mapper(pedalConfig());
    EXPECT_NEAR(mapper.map(withAxis(1, -1.0F)).throttle, 0.0F, kTolerance);
    EXPECT_NEAR(mapper.map(withAxis(1, 0.0F)).throttle, 0.5F, kTolerance);
    EXPECT_NEAR(mapper.map(withAxis(1, 1.0F)).throttle, 1.0F, kTolerance);
}

TEST(MapperPedals, InvertedAxis) {
    MapperConfig config = pedalConfig();
    config.brake.invert = true;
    const Mapper mapper(config);
    EXPECT_NEAR(mapper.map(withAxis(2, 1.0F)).brake, 0.0F, kTolerance);
    EXPECT_NEAR(mapper.map(withAxis(2, -1.0F)).brake, 1.0F, kTolerance);
}

TEST(MapperPedals, TheWholeTravelAndGamma) {
    MapperConfig config = pedalConfig();
    const Mapper linear(config);
    EXPECT_NEAR(linear.map(withAxis(2, -0.96F)).brake, 0.02F, kTolerance);   // no dead zone at the released end
    EXPECT_NEAR(linear.map(withAxis(2, 0.96F)).brake, 0.98F, kTolerance);    // nor early full at the other
    config.brake.gamma = 2.0F;
    const Mapper curved(config);
    EXPECT_NEAR(curved.map(withAxis(2, 0.0F)).brake, 0.25F, kTolerance);     // travel 0.5 -> 0.5^2
    EXPECT_NEAR(curved.map(withAxis(2, 1.0F)).brake, 1.0F, kTolerance);
}

TEST(MapperPedals, UnmappedPedalStaysReleased) {
    const Mapper mapper(MapperConfig{});
    EXPECT_EQ(mapper.map(withAxis(1, 1.0F)).throttle, 0.0F);
}

TEST(MapperButtons, IdentityAndRemap) {
    MapperConfig config;
    config.buttonMap[0] = 5;
    config.buttonMap[1] = kUnmapped;
    const Mapper mapper(config);

    RawInputState raw;
    raw.buttons = (1ULL << 5) | (1ULL << 1) | (1ULL << 2);
    const WheelState state = mapper.map(raw);
    EXPECT_EQ(state.buttons & 1U, 1U);          // target 0 <- source 5
    EXPECT_EQ(state.buttons & 2U, 0U);          // target 1 unmapped
    EXPECT_EQ(state.buttons & 4U, 4U);          // target 2 <- source 2 (identity)
}

TEST(MapperButtons, HatPassesThrough) {
    const Mapper mapper(MapperConfig{});
    RawInputState raw;
    raw.hat = 3;
    EXPECT_EQ(mapper.map(raw).hat, 3);
}

MapperConfig dpadAsButtons() {
    MapperConfig config;
    config.hatButtons.up = 4;
    config.hatButtons.right = 5;
    config.hatButtons.down = 6;
    config.hatButtons.left = 7;
    return config;
}

RawInputState withButtons(const std::uint64_t buttons) {
    RawInputState raw;
    raw.buttons = buttons;
    return raw;
}

TEST(MapperHat, ButtonsBecomeHatDirections) {
    const Mapper mapper(dpadAsButtons());
    EXPECT_EQ(mapper.map(withButtons(0)).hat, kHatCentered);
    EXPECT_EQ(mapper.map(withButtons(1U << 4)).hat, 0);                   // up
    EXPECT_EQ(mapper.map(withButtons(1U << 5)).hat, 2);                   // right
    EXPECT_EQ(mapper.map(withButtons(1U << 6)).hat, 4);                   // down
    EXPECT_EQ(mapper.map(withButtons(1U << 7)).hat, 6);                   // left
    EXPECT_EQ(mapper.map(withButtons((1U << 4) | (1U << 5))).hat, 1);     // up-right
    EXPECT_EQ(mapper.map(withButtons((1U << 6) | (1U << 7))).hat, 5);     // down-left
    EXPECT_EQ(mapper.map(withButtons((1U << 4) | (1U << 6))).hat, kHatCentered);  // up + down cancel
}

TEST(MapperHat, RealSourceHatWins) {
    const Mapper mapper(dpadAsButtons());
    RawInputState raw = withButtons(1U << 4);
    raw.hat = 6;
    EXPECT_EQ(mapper.map(raw).hat, 6);
}

TEST(MapperHat, WithoutHatButtonsTheSourceHatPassesThrough) {
    const Mapper mapper(MapperConfig{});
    RawInputState raw = withButtons(1U << 4);
    EXPECT_EQ(mapper.map(raw).hat, kHatCentered);
    raw.hat = 3;
    EXPECT_EQ(mapper.map(raw).hat, 3);
}

TEST(MapperDevices, AccessoriesDrivePedalsButtonsAndTheDpad) {
    MapperConfig config;
    config.accessories[0] = {0x044F, 0xB696};   // device 1: pedals
    config.accessories[1] = {0x1234, 0x0001};   // device 2: a button box
    config.throttle.sourceAxis = 1;
    config.throttle.sourceDevice = 1;
    config.buttonMap[4] = 3;
    config.buttonDevice[4] = 2;
    config.hatButtons.up = 0;
    config.hatButtons.devices[0] = 2;
    const Mapper mapper(config);

    RawInputs inputs;
    inputs.devices[0].axes[1] = 1.0F;           // the wheel's axis 1: not the throttle any more
    inputs.devices[0].buttons = 1U << 3;        // the wheel's button 3: drives target 3 only
    EXPECT_FLOAT_EQ(mapper.map(inputs).throttle, 0.5F);   // device 1's axis 1 at rest (0) = half travel
    EXPECT_EQ(mapper.map(inputs).buttons, 1U << 3);

    inputs.devices[1].axes[1] = 1.0F;
    inputs.devices[2].buttons = (1U << 3) | 1U;
    const WheelState state = mapper.map(inputs);
    EXPECT_FLOAT_EQ(state.throttle, 1.0F);
    EXPECT_EQ(state.buttons, (1U << 3) | (1U << 4));        // device 2's button 0 drives only the D-pad
    EXPECT_EQ(state.hat, 0);                                 // up, from the button box
}

}  // namespace
TEST(MapperDevices, MappedAxesTheDeviceDoesNotHaveAreNamed) {
    MapperConfig config;
    config.steering.sourceAxis = 0;
    config.throttle.sourceAxis = 2;
    config.brake.sourceAxis = 6;                 // the wheel has 3
    config.clutch.sourceAxis = 1;
    config.clutch.sourceDevice = 1;              // an accessory with 1 axis
    std::array<int, kMaxInputDevices> counts{};
    counts.fill(-1);
    counts[0] = 3;
    counts[1] = 1;
    const auto problems = missingMappedAxes(config, counts);
    ASSERT_EQ(problems.size(), 2U);
    EXPECT_EQ(problems[0], "brake: axis 6 of the wheel, which has 3 axes");
    EXPECT_EQ(problems[1], "clutch: axis 1 of accessory 1, which has 1 axis");
    counts[1] = -1;                              // not known: not checked
    EXPECT_EQ(missingMappedAxes(config, counts).size(), 1U);
    config.brake.sourceAxis = kUnmapped;
    EXPECT_TRUE(missingMappedAxes(config, counts).empty());
}

TEST(Mapper, ATargetButtonPressedByEitherSourceAndAnAxisPastItsThreshold) {
    MapperConfig config;
    config.buttonMap[2] = 2;                                  // G920 X <- the wheel's X
    config.buttonSecondary[2][0] = ButtonSource{1, 3, true, false, 0.5F};   // ... or the handbrake lever (device 1, axis 3)
    config.buttonSecondary[0][0] = ButtonSource{0, 20, false, false, 0.5F}; // G920 A <- also wheel button 20
    const Mapper mapper(config);
    RawInputs inputs;
    inputs.devices[1].axes[3] = -1.0F;   // released
    EXPECT_EQ(mapper.map(inputs).buttons & (1U << 2), 0U);
    inputs.devices[0].buttons = 1ULL << 2;   // the wheel's X
    EXPECT_NE(mapper.map(inputs).buttons & (1U << 2), 0U);
    inputs.devices[0].buttons = 1ULL << 20;   // button 20: G920 A
    EXPECT_NE(mapper.map(inputs).buttons & 1U, 0U);
    inputs.devices[0].buttons = 0;
    inputs.devices[1].axes[3] = 0.1F;   // 55% of the travel: past 50%
    EXPECT_NE(mapper.map(inputs).buttons & (1U << 2), 0U);
    inputs.devices[1].axes[3] = -0.05F;   // 47.5%: still pressed (released only below 45%)
    EXPECT_NE(mapper.map(inputs).buttons & (1U << 2), 0U);
    inputs.devices[1].axes[3] = -0.12F;   // 44%: released
    EXPECT_EQ(mapper.map(inputs).buttons & (1U << 2), 0U);
    inputs.devices[1].axes[3] = -0.05F;   // 47.5% from below: not yet
    EXPECT_EQ(mapper.map(inputs).buttons & (1U << 2), 0U);
}

TEST(Mapper, ManySecondariesEachAxisWithItsOwnRelease) {
    MapperConfig config;
    config.buttonMap[2] = kUnmapped;
    config.buttonSecondary[2] = {ButtonSource{0, 20, false, false, 0.5F}, ButtonSource{1, 4, false, false, 0.5F},
                                 ButtonSource{1, 3, true, false, 0.5F}, ButtonSource{0, 6, true, false, 0.75F}};
    const Mapper mapper(config);
    RawInputs inputs;
    inputs.devices[1].axes[3] = -1.0F;
    inputs.devices[0].axes[6] = -1.0F;
    const auto x = [&] { return (mapper.map(inputs).buttons & (1U << 2)) != 0; };
    EXPECT_FALSE(x());
    inputs.devices[1].buttons = 1ULL << 4;   // the button box's button
    EXPECT_TRUE(x());
    inputs.devices[1].buttons = 0;
    inputs.devices[0].axes[6] = 0.6F;   // 80%: past 75%
    inputs.devices[1].axes[3] = 0.2F;   // 60%: past 50%
    EXPECT_TRUE(x());
    inputs.devices[0].axes[6] = 0.42F;   // 71%: above its release (70%), still pressed
    inputs.devices[1].axes[3] = -0.2F;   // 40%: the lever is released
    EXPECT_TRUE(x());
    inputs.devices[0].axes[6] = 0.3F;    // 65%: released too
    EXPECT_FALSE(x());
    inputs.devices[0].buttons = 1ULL << 20;
    EXPECT_TRUE(x());
}

// The whole travel: 0% is pressed as soon as the axis moves (never at rest) and released only back at rest; 100% only
// at its end, released 5% below.
TEST(Mapper, AxisThresholdsAtBothEnds) {
    MapperConfig config;
    config.buttonMap[2] = kUnmapped;
    config.buttonMap[3] = kUnmapped;
    config.buttonSecondary[2][0] = ButtonSource{0, 5, true, false, 0.0F};
    config.buttonSecondary[3][0] = ButtonSource{0, 6, true, false, 1.0F};
    const Mapper mapper(config);
    RawInputs inputs;
    inputs.devices[0].axes[5] = -1.0F;   // at rest
    inputs.devices[0].axes[6] = -1.0F;
    const auto pressed = [&](const int b) { return (mapper.map(inputs).buttons & (1U << b)) != 0; };
    EXPECT_FALSE(pressed(2));
    inputs.devices[0].axes[5] = -0.98F;   // 1%: moved
    EXPECT_TRUE(pressed(2));
    inputs.devices[0].axes[5] = -1.0F;    // back at rest
    EXPECT_FALSE(pressed(2));
    inputs.devices[0].axes[6] = 0.98F;    // 99%: not yet
    EXPECT_FALSE(pressed(3));
    inputs.devices[0].axes[6] = 1.0F;     // the end
    EXPECT_TRUE(pressed(3));
    inputs.devices[0].axes[6] = 0.92F;    // 96%: still pressed (released below 95%)
    EXPECT_TRUE(pressed(3));
    inputs.devices[0].axes[6] = 0.8F;     // 90%
    EXPECT_FALSE(pressed(3));
}

TEST(Mapper, AnInvertedAxisCountsFromWhereItRests) {
    MapperConfig config;
    config.buttonMap[2] = kUnmapped;
    config.buttonSecondary[2][0] = ButtonSource{0, 5, true, true, 0.5F};   // rests at +1
    const Mapper mapper(config);
    RawInputs inputs;
    inputs.devices[0].axes[5] = 1.0F;
    EXPECT_EQ(mapper.map(inputs).buttons & (1U << 2), 0U);
    inputs.devices[0].axes[5] = -0.5F;   // 75% pulled
    EXPECT_NE(mapper.map(inputs).buttons & (1U << 2), 0U);
}

}  // namespace wheelbri

namespace wheelbri {
namespace {

// An axis that reads NaN (a broken driver, a bad float from an accessory): steering centred, pedals released, never NaN
// in the WheelState the window, telemetry and the encoders read.
TEST(MapperAxes, NotANumberReadsAsRest) {
    MapperConfig config;
    config.throttle.sourceAxis = 1;
    config.brake.sourceAxis = 2;
    config.brake.invert = true;
    config.clutch.sourceAxis = 3;
    config.clutch.gamma = 2.0F;
    const Mapper mapper(config);
    RawInputState raw;
    raw.axes.fill(std::numeric_limits<float>::quiet_NaN());
    const WheelState state = mapper.map(raw);
    EXPECT_EQ(state.steering, 0.0F);
    EXPECT_EQ(state.throttle, 0.0F);
    EXPECT_EQ(state.brake, 0.0F);
    EXPECT_EQ(state.clutch, 0.0F);
}

// Infinity and values past the ends (a driver that overshoots its declared range): clamped to the ends.
TEST(MapperAxes, InfinityAndOvershootAreClampedToTheEnds) {
    MapperConfig config = pedalConfig();
    config.brake.invert = true;
    const Mapper mapper(config);
    RawInputState raw;
    raw.axes[0] = std::numeric_limits<float>::infinity();
    raw.axes[1] = 7.0F;
    raw.axes[2] = -std::numeric_limits<float>::infinity();
    raw.axes[3] = -3.0F;
    const WheelState state = mapper.map(raw);
    EXPECT_EQ(state.steering, 1.0F);
    EXPECT_EQ(state.throttle, 1.0F);
    EXPECT_EQ(state.brake, 1.0F);    // inverted: -inf is the pressed end
    EXPECT_EQ(state.clutch, 0.0F);
}

// Inverted, gamma-curved pedals at both ends: exactly 0 released and exactly 1 pressed, whatever the curve.
TEST(MapperPedals, InvertedCurvedPedalHitsBothEndsExactly) {
    for (const float gamma : {0.3F, 1.0F, 2.5F}) {
        MapperConfig config = pedalConfig();
        config.throttle.invert = true;
        config.throttle.gamma = gamma;
        const Mapper mapper(config);
        EXPECT_EQ(mapper.map(withAxis(1, 1.0F)).throttle, 0.0F) << gamma;
        EXPECT_EQ(mapper.map(withAxis(1, -1.0F)).throttle, 1.0F) << gamma;
        const float half = mapper.map(withAxis(1, 0.0F)).throttle;
        EXPECT_NEAR(half, std::pow(0.5F, gamma), kTolerance) << gamma;
    }
}

// A config whose axis or device number is outside what the service reads (hand-edited, or from a newer version):
// the control stays at rest instead of reading past the arrays.
TEST(MapperAxes, OutOfRangeAxisOrDeviceStaysAtRest) {
    MapperConfig config;
    config.steering.sourceAxis = static_cast<int>(RawInputState::kMaxAxes);   // warned about, stays centred
    config.throttle.sourceAxis = 1;
    config.throttle.sourceDevice = static_cast<int>(kMaxInputDevices);
    config.brake.sourceAxis = 2;
    config.brake.sourceDevice = -1;
    config.clutch.sourceAxis = -5;
    const Mapper mapper(config);
    RawInputs inputs;
    for (RawInputState& device : inputs.devices) device.axes.fill(1.0F);
    const WheelState state = mapper.map(inputs);
    EXPECT_EQ(state.steering, 0.0F);
    EXPECT_EQ(state.throttle, 0.0F);
    EXPECT_EQ(state.brake, 0.0F);
    EXPECT_EQ(state.clutch, 0.0F);

    MapperConfig unmapped;
    unmapped.steering.sourceAxis = kUnmapped;
    EXPECT_EQ(Mapper(unmapped).map(withAxis(0, 1.0F)).steering, 0.0F);
}

// Source buttons 32..63 (a button box) can drive target buttons; numbers past 63, negative ones, or on a device the
// session does not read press nothing.
TEST(MapperButtons, HighSourceButtonsAndOutOfRangeNumbers) {
    MapperConfig config;
    config.buttonMap[0] = 63;
    config.buttonMap[1] = 64;
    config.buttonMap[2] = 40;
    config.buttonDevice[2] = static_cast<int>(kMaxInputDevices);
    config.buttonMap[3] = 3;
    config.buttonDevice[3] = -1;
    config.buttonSecondary[4][0] = ButtonSource{0, 70, false, false, 0.5F};
    config.buttonSecondary[5][0] = ButtonSource{9, 1, false, false, 0.5F};
    config.buttonMap[4] = kUnmapped;
    config.buttonMap[5] = kUnmapped;
    const Mapper mapper(config);
    RawInputState raw;
    raw.buttons = ~std::uint64_t{0};
    const std::uint32_t buttons = mapper.map(raw).buttons;
    EXPECT_NE(buttons & (1U << 0), 0U);   // source 63
    EXPECT_EQ(buttons & (1U << 1), 0U);   // 64: past the source buttons
    EXPECT_EQ(buttons & (1U << 2), 0U);   // device 4: not read
    EXPECT_EQ(buttons & (1U << 3), 0U);   // device -1
    EXPECT_EQ(buttons & (1U << 4), 0U);   // secondary button 70
    EXPECT_EQ(buttons & (1U << 5), 0U);   // secondary on device 9
    EXPECT_NE(buttons & (1U << 31), 0U);  // the last target button, identity
}

// A secondary axis source on an axis that does not exist reads as resting: never pressed, inverted or not.
TEST(MapperButtons, AxisSecondaryOnAMissingAxisNeverPresses) {
    MapperConfig config;
    config.buttonMap[6] = kUnmapped;
    config.buttonMap[7] = kUnmapped;
    config.buttonSecondary[6][0] = ButtonSource{0, 12, true, false, 0.1F};   // axis 12: there is no such axis
    config.buttonSecondary[7][0] = ButtonSource{0, 12, true, true, 0.1F};
    config.buttonSecondary[7][1] = ButtonSource{0, 2, true, false, 0.5F};    // NaN axis: released
    const Mapper mapper(config);
    RawInputState raw;
    raw.axes[2] = std::numeric_limits<float>::quiet_NaN();
    const std::uint32_t buttons = mapper.map(raw).buttons;
    EXPECT_EQ(buttons & (1U << 6), 0U);
    EXPECT_EQ(buttons & (1U << 7), 0U);
    raw.axes[2] = 1.0F;                   // the lever pulled, then NaN: released, not stuck pressed
    EXPECT_NE(mapper.map(raw).buttons & (1U << 7), 0U);
    raw.axes[2] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(mapper.map(raw).buttons & (1U << 7), 0U);
}

// A D-pad from buttons with only some directions mapped, one of them also a target button (warned about at start).
TEST(MapperHat, PartialDpadFromButtons) {
    MapperConfig config;
    config.hatButtons.left = 3;   // only left; it also drives target 3 (identity)
    const Mapper mapper(config);
    const WheelState state = mapper.map(withButtons(1U << 3));
    EXPECT_EQ(state.hat, 6);
    EXPECT_NE(state.buttons & (1U << 3), 0U);
    EXPECT_EQ(mapper.map(withButtons(0)).hat, kHatCentered);

    MapperConfig downOnly;
    downOnly.hatButtons.down = 40;
    EXPECT_EQ(Mapper(downOnly).map(withButtons(1ULL << 40)).hat, 4);
    MapperConfig rightOnly;
    rightOnly.hatButtons.right = 1;
    EXPECT_EQ(Mapper(rightOnly).map(withButtons(1U << 1)).hat, 2);
}

// Checking mapped axes against what the devices have: a device number outside the session, or an unknown count,
// is not checked; one axis is spelled "axis", an accessory by its number.
TEST(MapperDevices, MissingAxesSkipsUnknownDevicesAndSpellsCounts) {
    MapperConfig config;
    config.steering.sourceAxis = 1;              // the wheel has 1 axis
    config.throttle.sourceAxis = 0;
    config.throttle.sourceDevice = 7;            // not a device the session reads
    config.brake.sourceAxis = 0;
    config.brake.sourceDevice = -1;
    config.clutch.sourceAxis = 4;
    config.clutch.sourceDevice = 3;              // accessory 3 has 4 axes
    std::array<int, kMaxInputDevices> counts{1, -1, -1, 4};
    const auto problems = missingMappedAxes(config, counts);
    ASSERT_EQ(problems.size(), 2U);
    EXPECT_EQ(problems[0], "steering: axis 1 of the wheel, which has 1 axis");
    EXPECT_EQ(problems[1], "clutch: axis 4 of accessory 3, which has 4 axes");
}

TEST(MapperConfig, AccessoriesEndAtTheFirstEmptySlotAndIdsAreCompared) {
    MapperConfig config;
    EXPECT_EQ(config.accessoryCount(), 0U);
    config.accessories[0] = {0x0000, 0x0001};    // vendor 0 but a product: not empty
    EXPECT_FALSE(config.accessories[0].empty());
    config.accessories[1] = {0x044F, 0x0000};
    config.accessories[2] = {0x1234, 0x5678};
    EXPECT_EQ(config.accessoryCount(), 3U);       // all three used
    config.accessories[1] = {};
    EXPECT_EQ(config.accessoryCount(), 1U);
    EXPECT_EQ(config.accessories[1], DeviceId{});
    EXPECT_FALSE(config.accessories[0] == config.accessories[2]);

    const ButtonSource a{1, 2, true, false, 0.5F};
    EXPECT_EQ(a, (ButtonSource{1, 2, true, false, 0.5F}));
    EXPECT_FALSE(a == (ButtonSource{2, 2, true, false, 0.5F}));
    EXPECT_FALSE(a == (ButtonSource{1, 3, true, false, 0.5F}));
    EXPECT_FALSE(a == (ButtonSource{1, 2, false, false, 0.5F}));
    EXPECT_FALSE(a == (ButtonSource{1, 2, true, true, 0.5F}));
    EXPECT_FALSE(a == (ButtonSource{1, 2, true, false, 0.6F}));

    HatButtons hat;
    EXPECT_FALSE(hat.any());
    hat.down = 0;
    EXPECT_TRUE(hat.any());
}

// RawInputState compares every field: a hat or button change alone counts as new input (Pipeline::inputChanged).
TEST(MapperConfig, RawInputStateComparesEveryField) {
    RawInputState a;
    RawInputState b;
    EXPECT_EQ(a, b);
    b.buttons = 1;
    EXPECT_FALSE(a == b);
    b = a;
    b.hat = 0;
    EXPECT_FALSE(a == b);
    b = a;
    b.axes[7] = 0.5F;
    EXPECT_FALSE(a == b);
}

}  // namespace
}  // namespace wheelbri
