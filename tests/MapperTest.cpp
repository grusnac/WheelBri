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

TEST(MapperSteering, DeadzoneIsRemovedWithoutAJump) {
    MapperConfig config;
    config.steering.deadzone = 0.1F;
    const Mapper mapper(config);
    EXPECT_EQ(mapper.map(withAxis(0, 0.05F)).steering, 0.0F);
    EXPECT_NEAR(mapper.map(withAxis(0, 0.1F + 1e-6F)).steering, 0.0F, 1e-4F);
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

TEST(MapperPedals, DeadzoneSaturationAndGamma) {
    MapperConfig config = pedalConfig();
    config.brake.deadzoneLow = 0.1F;
    config.brake.saturationHigh = 0.9F;
    config.brake.gamma = 2.0F;
    const Mapper mapper(config);
    EXPECT_NEAR(mapper.map(withAxis(2, -0.9F)).brake, 0.0F, kTolerance);   // travel 0.05, inside dead zone
    EXPECT_NEAR(mapper.map(withAxis(2, 0.0F)).brake, 0.25F, kTolerance);   // travel 0.5 -> 0.5 -> 0.5^2
    EXPECT_NEAR(mapper.map(withAxis(2, 0.85F)).brake, 1.0F, kTolerance);   // travel 0.925, saturated
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
}  // namespace wheelbri
