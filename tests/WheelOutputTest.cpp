#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EvdevLayout.hpp"
#include "wheelbri/WheelOutput.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace wheelbri {
namespace {

FfbEffect constant(const float level) {
    FfbEffect e;
    e.type = FfbEffectType::Constant;
    e.constantLevel = level;
    return e;
}

FfbEffect spring() {
    FfbEffect e;
    e.type = FfbEffectType::Spring;
    e.condition.center = 0.1F;
    e.condition.deadband = 0.05F;
    e.condition.positiveCoefficient = 0.5F;
    e.condition.negativeCoefficient = 0.25F;
    e.condition.positiveSaturation = 1.0F;
    e.condition.negativeSaturation = 0.5F;
    return e;
}

TEST(WheelEffect, SameNumbersAsSdlHaptic) {
    EXPECT_EQ(toWheelEffect(constant(0.5F), false).level, 16384);    // lround(0.5 * 32767)
    EXPECT_EQ(toWheelEffect(constant(-2.0F), false).level, -32767);  // clamped
    EXPECT_EQ(toWheelEffect(constant(0.5F), true).level, -16384);

    const WheelEffect s = toWheelEffect(spring(), false);
    EXPECT_EQ(s.rightCoefficient, 16384);
    EXPECT_EQ(s.leftCoefficient, 8192);
    EXPECT_EQ(s.rightSaturation, 65535);
    EXPECT_EQ(s.leftSaturation, 32768);
    EXPECT_EQ(s.center, 3277);
    const WheelEffect mirrored = toWheelEffect(spring(), true);
    EXPECT_EQ(mirrored.rightCoefficient, s.leftCoefficient);
    EXPECT_EQ(mirrored.leftSaturation, s.rightSaturation);
    EXPECT_EQ(mirrored.center, -3277);

    FfbEffect sine;
    sine.type = FfbEffectType::Sine;
    sine.periodic = {0.4F, -0.1F, 50.0F, -90.0F};
    const WheelEffect w = toWheelEffect(sine, false);
    EXPECT_EQ(w.periodMs, 50);
    EXPECT_EQ(w.magnitude, 13107);
    EXPECT_EQ(w.offset, -3277);
    EXPECT_EQ(w.phase, 27000);
}

TEST(WheelEffect, DirectInputLikeSdl) {
    const di::Output c = di::toDirectInput(toWheelEffect(constant(1.0F), false));
    EXPECT_EQ(c.block, di::Block::Constant);
    EXPECT_EQ(c.magnitude, 10000);
    EXPECT_EQ(di::toDirectInput(toWheelEffect(constant(-0.5F), false)).magnitude, -5000);   // -16384*10000/32767

    const di::Output s = di::toDirectInput(toWheelEffect(spring(), false));
    EXPECT_EQ(s.block, di::Block::Condition);
    EXPECT_EQ(s.positiveCoefficient, 5000);
    EXPECT_EQ(s.negativeCoefficient, 2500);
    EXPECT_EQ(s.positiveSaturation, 10000U);   // 65535 / 2 = 32767 -> 10000
    EXPECT_EQ(s.negativeSaturation, 5000U);
    EXPECT_EQ(s.offset, 1000);

    WheelEffect p;
    p.type = FfbEffectType::Square;
    p.magnitude = -16384;
    p.phase = 27000;
    p.periodMs = 20;
    const di::Output d = di::toDirectInput(p);
    EXPECT_EQ(d.block, di::Block::Periodic);
    EXPECT_EQ(d.periodicMagnitude, 5000U);
    EXPECT_EQ(d.phase, 9000U);                 // negative magnitude: half a turn on
    EXPECT_EQ(d.periodUs, 20000U);
}

TEST(WheelEffect, EvdevLikeSdl) {
    const evdev::Output c = evdev::toEvdev(toWheelEffect(constant(0.25F), false));
    EXPECT_EQ(c.type, evdev::kFfConstant);
    EXPECT_EQ(c.direction, 0x4000);
    EXPECT_EQ(c.level, 8192);

    FfbEffect damper = spring();
    damper.type = FfbEffectType::Damper;
    const evdev::Output d = evdev::toEvdev(toWheelEffect(damper, false));
    EXPECT_EQ(d.type, evdev::kFfDamper);
    EXPECT_EQ(d.rightCoefficient, 16384);
    EXPECT_EQ(d.leftSaturation, 32768);

    WheelEffect p;
    p.type = FfbEffectType::SawtoothDown;
    p.phase = 9000;
    p.periodMs = 40000;
    const evdev::Output w = evdev::toEvdev(p);
    EXPECT_EQ(w.type, evdev::kFfPeriodic);
    EXPECT_EQ(w.waveform, evdev::kFfSawDown);
    EXPECT_EQ(w.phase, 0x4000);                // 90 degrees
    EXPECT_EQ(w.periodMs, 32767);              // SDL's CLAMP
}

TEST(DeviceMatch, NameOrUsbId) {
    EXPECT_TRUE(deviceMatches("MY RACING Wheel Base", 0x346e, 0x0005, "racing"));
    EXPECT_TRUE(deviceMatches("Wheel", 0x346e, 0x0005, "346E"));
    EXPECT_TRUE(deviceMatches("Wheel", 0x346e, 0x0005, "346e:0005"));
    EXPECT_FALSE(deviceMatches("Logitech G920", 0x046d, 0xc262, "346e"));
}

evdev::Capabilities wheelCaps() {
    evdev::Capabilities caps;
    caps.keys.set(0x120);   // BTN_TRIGGER
    caps.keys.set(0x121);
    caps.keys.set(0x2c0);   // BTN_TRIGGER_HAPPY1
    caps.keys.set(0x110);   // BTN_LEFT, below BTN_JOYSTICK: numbered after the others
    for (const int code : {0x00, 0x02, 0x05, 0x10, 0x11}) {   // X, Z, RZ, HAT0X, HAT0Y
        caps.axes.set(static_cast<std::size_t>(code));
    }
    caps.absInfo[0x00] = {0, 0, 65535, 0, 0, 0};
    caps.absInfo[0x02] = {0, 0, 4095, 0, 0, 0};
    caps.absInfo[0x05] = {0, -512, 511, 0, 0, 0};
    caps.absInfo[0x10] = {0, -1, 1, 0, 0, 0};
    caps.absInfo[0x11] = {0, -1, 1, 0, 0, 0};
    return caps;
}

TEST(EvdevLayout, NumbersLikeSdl) {
    const evdev::Capabilities caps = wheelCaps();
    EXPECT_TRUE(evdev::isGameController(caps));
    evdev::Layout layout(caps);
    EXPECT_EQ(layout.axes(), 3);
    EXPECT_EQ(layout.buttons(), 4);
    EXPECT_EQ(layout.hats(), 1);

    layout.apply(evdev::kEvKey, 0x110, 1);    // BTN_LEFT is button 3
    layout.apply(evdev::kEvKey, 0x2c0, 1);    // TRIGGER_HAPPY1 is button 2
    layout.apply(evdev::kEvAbs, 0x00, 65535);
    layout.apply(evdev::kEvAbs, 0x02, 0);
    layout.apply(evdev::kEvAbs, 0x05, 0);
    layout.apply(evdev::kEvAbs, 0x10, 1);     // right
    layout.apply(evdev::kEvAbs, 0x11, -1);    // up
    RawInputState state;
    layout.fill(state);
    EXPECT_EQ(state.buttons, (1ULL << 3) | (1ULL << 2));
    EXPECT_FLOAT_EQ(state.axes[0], 1.0F);
    EXPECT_FLOAT_EQ(state.axes[1], -1.0F);
    EXPECT_EQ(layout.axisValue(2), 32);       // floor(512 * 65535/1023 - 32768 + 0.5): SDL's rounding
    EXPECT_EQ(state.hat, 1);                  // up-right

    layout.apply(evdev::kEvKey, 0x110, 0);
    layout.apply(evdev::kEvAbs, 0x11, 0);
    layout.fill(state);
    EXPECT_EQ(state.buttons, 1ULL << 2);
    EXPECT_EQ(state.hat, 2);                  // right
    EXPECT_TRUE(layout.apply(evdev::kEvSyn, evdev::kSynDropped, 0));
}

TEST(EvdevLayout, KeyboardsMiceAndTouchPadsAreNotControllers) {
    evdev::Capabilities keyboard;
    keyboard.keys.set(30);   // KEY_A
    EXPECT_FALSE(evdev::isGameController(keyboard));

    evdev::Capabilities touchpad;
    touchpad.axes.set(0x00);
    touchpad.axes.set(0x01);
    touchpad.keys.set(0x110);   // BTN_LEFT
    touchpad.keys.set(0x14a);   // BTN_TOUCH
    EXPECT_FALSE(evdev::isGameController(touchpad));

    evdev::Capabilities mouse = wheelCaps();
    mouse.hasRelative = true;
    EXPECT_FALSE(evdev::isGameController(mouse));

    evdev::Capabilities pedals;   // no buttons, only driving axes
    pedals.axes.set(0x09);        // ABS_GAS
    pedals.axes.set(0x0a);        // ABS_BRAKE
    EXPECT_TRUE(evdev::isGameController(pedals));
}

TEST(WheelEffect, NonFiniteValuesGiveNoForce) {
    FfbEffect e = constant(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(toWheelEffect(e, false).level, 0);
}

}  // namespace
}  // namespace wheelbri
