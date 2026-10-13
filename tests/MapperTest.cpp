#include "wheelbri/Mapper.hpp"

#include <gtest/gtest.h>

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

TEST(MapperSteering, SmallerSourceRotationKeepsDegreesOneToOne) {
    MapperConfig config;
    config.steering.sourceRotationDeg = 540.0F;
    config.steering.targetRotationDeg = 900.0F;
    const Mapper mapper(config);
    // Full lock on a 540-degree wheel is 270 of the target's 450 degrees each side.
    EXPECT_NEAR(mapper.map(withAxis(0, 1.0F)).steering, 0.6F, kTolerance);
}

TEST(MapperSteering, LargerSourceRotationClampsAtTargetLock) {
    MapperConfig config;
    config.steering.sourceRotationDeg = 1080.0F;
    config.steering.targetRotationDeg = 900.0F;
    const Mapper mapper(config);
    EXPECT_NEAR(mapper.map(withAxis(0, 1.0F)).steering, 1.0F, kTolerance);
    EXPECT_NEAR(mapper.map(withAxis(0, 0.5F)).steering, 0.6F, kTolerance);
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
