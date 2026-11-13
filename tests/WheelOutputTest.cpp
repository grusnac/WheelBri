// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/EvdevLayout.hpp"
#include "wheelbri/WheelOutput.hpp"

#include <gtest/gtest.h>

#include <bitset>
#include <cstdint>

#include <limits>
#include <utility>

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
    EXPECT_EQ(state.buttons, SourceButtons::fromBits((1ULL << 3) | (1ULL << 2)));
    EXPECT_FLOAT_EQ(state.axes[0], 1.0F);
    EXPECT_FLOAT_EQ(state.axes[1], -1.0F);
    EXPECT_EQ(layout.axisValue(2), 32);       // floor(512 * 65535/1023 - 32768 + 0.5): SDL's rounding
    EXPECT_EQ(state.hat, 1);                  // up-right

    layout.apply(evdev::kEvKey, 0x110, 0);
    layout.apply(evdev::kEvAbs, 0x11, 0);
    layout.fill(state);
    EXPECT_EQ(state.buttons, SourceButtons::fromBits(1ULL << 2));
    EXPECT_EQ(state.hat, 2);                  // right
    EXPECT_TRUE(layout.apply(evdev::kEvSyn, evdev::kSynDropped, 0));
}

TEST(EvdevLayout, ExtremeRangesStayInRange) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    for (const int code : {0x00, 0x01, 0x02}) caps.axes.set(static_cast<std::size_t>(code));
    caps.absInfo[0x00] = {0, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), 0, 0, 0};
    caps.absInfo[0x01] = {0, 5, 5, 0, 0, 0};       // no range at all
    caps.absInfo[0x02] = {0, 100, -100, 0, 0, 0};  // backwards
    evdev::Layout layout(caps);
    for (const std::int32_t value : {std::numeric_limits<std::int32_t>::min(), 0, std::numeric_limits<std::int32_t>::max()}) {
        layout.apply(evdev::kEvAbs, 0x00, value);
        layout.apply(evdev::kEvAbs, 0x01, value);
        layout.apply(evdev::kEvAbs, 0x02, value);
        RawInputState state;
        layout.fill(state);
        for (int axis = 0; axis < 3; ++axis) {
            EXPECT_GE(state.axes[static_cast<std::size_t>(axis)], -1.0F);
            EXPECT_LE(state.axes[static_cast<std::size_t>(axis)], 1.0F);
            EXPECT_GE(layout.axisValue(axis), -32768);
            EXPECT_LE(layout.axisValue(axis), 32767);
        }
    }
    layout.apply(evdev::kEvAbs, 0x00, std::numeric_limits<std::int32_t>::max());
    RawInputState state;
    layout.fill(state);
    EXPECT_FLOAT_EQ(state.axes[0], 1.0F);
}

// A hat whose range is wider than -1..1 (SDL's dead zone: a third of the range), and one that reports beyond it.
TEST(EvdevLayout, HatsUseAThirdOfTheirRangeAndGrowWithIt) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    caps.axes.set(0x10);
    caps.axes.set(0x11);
    caps.absInfo[0x10] = {0, -3, 3, 0, 0, 0};   // plain (no fuzz, flat, resolution): digital
    caps.absInfo[0x11] = {0, -3, 3, 0, 0, 0};
    evdev::Layout layout(caps);
    ASSERT_EQ(layout.hats(), 1);
    EXPECT_EQ(layout.axes(), 0);   // the pair is the hat, not two axes
    RawInputState state;
    const auto hatAt = [&](std::int32_t x, std::int32_t y) {
        layout.apply(evdev::kEvAbs, 0x10, x);
        layout.apply(evdev::kEvAbs, 0x11, y);
        layout.fill(state);
        return state.hat;
    };
    EXPECT_EQ(hatAt(0, 0), kHatCentered);
    EXPECT_EQ(hatAt(1, -1), kHatCentered);   // within the third: centred
    EXPECT_EQ(hatAt(2, 0), 2);               // right
    EXPECT_EQ(hatAt(0, 2), 4);               // down
    EXPECT_EQ(hatAt(-2, -2), 7);             // up-left
    EXPECT_EQ(hatAt(3, 3), 3);               // the ends: down-right
    EXPECT_EQ(hatAt(-9, 0), 6);              // beyond the range: left, and the range grows to -9
    EXPECT_EQ(hatAt(-2, 0), kHatCentered);   // -2 is now within a third of -9
    EXPECT_EQ(hatAt(-4, 0), 6);
}

TEST(EvdevLayout, AHatPairThatLooksAnalogueStaysTwoAxes) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    caps.axes.set(0x10);
    caps.axes.set(0x11);
    caps.absInfo[0x10] = {0, -127, 127, 4, 0, 0};   // fuzz: an analogue stick on the hat codes
    caps.absInfo[0x11] = {0, -127, 127, 4, 0, 0};
    evdev::Layout layout(caps);
    EXPECT_EQ(layout.hats(), 0);
    EXPECT_EQ(layout.axes(), 2);
    layout.apply(evdev::kEvAbs, 0x10, 127);
    RawInputState state;
    layout.fill(state);
    EXPECT_FLOAT_EQ(state.axes[0], 1.0F);
    EXPECT_EQ(state.hat, kHatCentered);
}

TEST(EvdevLayout, AHatWithOnlyOneAxisWorks) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    caps.axes.set(0x10);   // HAT0X only
    caps.absInfo[0x10] = {0, -1, 1, 0, 0, 0};
    evdev::Layout layout(caps);
    ASSERT_EQ(layout.hats(), 1);
    layout.apply(evdev::kEvAbs, 0x10, -1);
    RawInputState state;
    layout.fill(state);
    EXPECT_EQ(state.hat, 6);   // left
}

// Button boxes and wheels with more buttons than RawInputState holds: the first 128 count, the rest are ignored.
// (hid-input itself stops at 80 for a joystick; a device driver of its own may report more key codes.)
TEST(EvdevLayout, ButtonsBeyond128AreIgnoredSafely) {
    evdev::Capabilities caps;
    for (int code = 0x120; code < 0x120 + 30; ++code) caps.keys.set(static_cast<std::size_t>(code));   // 0..29
    for (int code = 0x2c0; code < 0x2ff; ++code) caps.keys.set(static_cast<std::size_t>(code));        // 30..92 (SDL: < KEY_MAX)
    for (int code = 1; code <= 50; ++code) caps.keys.set(static_cast<std::size_t>(code));              // 93..142 (SDL: after)
    caps.axes.set(0x00);
    caps.absInfo[0x00] = {0, 0, 255, 0, 0, 0};
    evdev::Layout layout(caps);
    ASSERT_EQ(layout.buttons(), 142 + 1);
    layout.apply(evdev::kEvKey, 36, 1);           // button 128: past 127
    layout.apply(evdev::kEvKey, 0x2c0 + 33, 1);   // button 63
    layout.apply(evdev::kEvKey, 0x2c0 + 34, 1);   // button 64: the first past the old limit
    layout.apply(evdev::kEvKey, 35, 1);           // button 127: the last one held
    RawInputState state;
    layout.fill(state);
    EXPECT_EQ(state.buttons, SourceButtons::fromBits(1ULL << 63, 1ULL | (1ULL << 63)));
    layout.apply(evdev::kEvKey, 35, 0);
    layout.fill(state);
    EXPECT_FALSE(state.buttons.test(127));

    std::bitset<evdev::kKeyCount> down;
    down.set(36);
    down.set(35);
    down.set(0x120);
    layout.reset(caps.absInfo, down);
    layout.fill(state);
    EXPECT_EQ(state.buttons, SourceButtons::fromBits(1ULL, 1ULL << 63));   // buttons 0 and 127
}

TEST(EvdevLayout, OnlyTheFirstEightAxesAreFilled) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    for (int code = 0; code < 10; ++code) {   // X .. ABS_BRAKE: ten axes
        caps.axes.set(static_cast<std::size_t>(code));
        caps.absInfo[static_cast<std::size_t>(code)] = {0, 0, 100, 0, 0, 0};
    }
    evdev::Layout layout(caps);
    ASSERT_EQ(layout.axes(), 10);
    layout.apply(evdev::kEvAbs, 9, 100);   // the tenth: readable, not in RawInputState
    layout.apply(evdev::kEvAbs, 7, 100);
    RawInputState state;
    layout.fill(state);
    EXPECT_FLOAT_EQ(state.axes[7], 1.0F);
    EXPECT_EQ(layout.axisValue(9), 32767);
}

TEST(EvdevLayout, ResetTakesTheDevicesValuesAfterDroppedEvents) {
    evdev::Capabilities caps = wheelCaps();
    evdev::Layout layout(caps);
    layout.apply(evdev::kEvKey, 0x120, 1);
    layout.apply(evdev::kEvAbs, 0x10, 1);
    auto now = caps.absInfo;
    now[0x00].value = 0;   // the wheel full left now
    now[0x10].value = 0;   // the hat released while events were lost
    now[0x11].value = 1;   // and pressed down
    std::bitset<evdev::kKeyCount> down;
    down.set(0x121);       // button 1 held instead of 0
    layout.reset(now, down);
    RawInputState state;
    layout.fill(state);
    EXPECT_FLOAT_EQ(state.axes[0], -1.0F);   // -32768 / 32767, clamped
    EXPECT_EQ(state.buttons, SourceButtons::fromBits(1ULL << 1));
    EXPECT_EQ(state.hat, 4);
}

TEST(EvdevLayout, TouchScreensPensAndBareAxesAreNotControllers) {
    evdev::Capabilities touchscreen;   // multi-touch axes only
    touchscreen.axes.set(0x2f);
    touchscreen.axes.set(0x35);
    touchscreen.keys.set(0x120);
    EXPECT_FALSE(evdev::isGameController(touchscreen));

    evdev::Capabilities pen;   // X/Y with BTN_TOOL_PEN (BTN_DIGI range)
    pen.axes.set(0x00);
    pen.axes.set(0x01);
    pen.keys.set(0x140);
    EXPECT_FALSE(evdev::isGameController(pen));

    evdev::Capabilities accelerometer;   // axes, no buttons, no driving axes
    accelerometer.axes.set(0x00);
    accelerometer.axes.set(0x01);
    accelerometer.axes.set(0x02);
    EXPECT_FALSE(evdev::isGameController(accelerometer));

    evdev::Capabilities numbered;   // a joystick numbering its buttons BTN_0..
    numbered.axes.set(0x00);
    numbered.keys.set(0x100 + 9);
    EXPECT_TRUE(evdev::isGameController(numbered));

    evdev::Capabilities buttonBox;   // trigger-happy buttons and one axis
    buttonBox.axes.set(0x00);
    buttonBox.keys.set(0x2c0 + 0x27);
    EXPECT_TRUE(evdev::isGameController(buttonBox));

    evdev::Capabilities buttonsOnly;   // buttons without any axis
    buttonsOnly.keys.set(0x120);
    EXPECT_FALSE(evdev::isGameController(buttonsOnly));
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

FfbEffect wave(const FfbEffectType type, const FfbPeriodic periodic) {
    FfbEffect e;
    e.type = type;
    e.periodic = periodic;
    return e;
}

// Every condition type reaches DirectInput as a condition block and evdev as its own effect type, with the same
// numbers (friction and inertia are played as games ask, not only springs and dampers).
TEST(WheelEffect, EveryConditionTypeConverts) {
    const std::pair<FfbEffectType, std::uint16_t> types[] = {{FfbEffectType::Spring, evdev::kFfSpring},
                                                             {FfbEffectType::Damper, evdev::kFfDamper},
                                                             {FfbEffectType::Friction, evdev::kFfFriction},
                                                             {FfbEffectType::Inertia, evdev::kFfInertia}};
    for (const auto& [type, evdevType] : types) {
        FfbEffect e = spring();
        e.type = type;
        const WheelEffect w = toWheelEffect(e, false);
        EXPECT_EQ(w.type, type);
        EXPECT_EQ(w.deadband, 3277);   // lround(0.05 * 65535)
        const di::Output d = di::toDirectInput(w);
        EXPECT_EQ(d.block, di::Block::Condition);
        EXPECT_EQ(d.positiveCoefficient, 5000);
        EXPECT_EQ(d.negativeCoefficient, 2500);
        EXPECT_EQ(d.deadBand, 499);    // 3277 / 2 = 1638 -> 1638 * 10000 / 32767
        const evdev::Output v = evdev::toEvdev(w);
        EXPECT_EQ(v.type, evdevType);
        EXPECT_EQ(v.rightCoefficient, 16384);
        EXPECT_EQ(v.leftCoefficient, 8192);
        EXPECT_EQ(v.rightSaturation, 65535);
        EXPECT_EQ(v.deadband, 3277);
        EXPECT_EQ(v.center, 3277);
    }
}

// Every periodic type reaches DirectInput as a periodic block and evdev with its own waveform.
TEST(WheelEffect, EveryPeriodicTypeConverts) {
    const std::pair<FfbEffectType, std::uint16_t> types[] = {{FfbEffectType::Sine, evdev::kFfSine},
                                                             {FfbEffectType::Square, evdev::kFfSquare},
                                                             {FfbEffectType::Triangle, evdev::kFfTriangle},
                                                             {FfbEffectType::SawtoothUp, evdev::kFfSawUp},
                                                             {FfbEffectType::SawtoothDown, evdev::kFfSawDown}};
    for (const auto& [type, waveform] : types) {
        const WheelEffect w = toWheelEffect(wave(type, {0.5F, 0.25F, 100.0F, 90.0F}), false);
        EXPECT_EQ(w.magnitude, 16384);
        EXPECT_EQ(w.offset, 8192);
        EXPECT_EQ(w.phase, 9000);
        EXPECT_EQ(w.periodMs, 100);
        const di::Output d = di::toDirectInput(w);
        EXPECT_EQ(d.block, di::Block::Periodic);
        EXPECT_EQ(d.periodicMagnitude, 5000U);
        EXPECT_EQ(d.periodicOffset, 2500);
        EXPECT_EQ(d.phase, 9000U);     // positive magnitude: the phase as it is
        EXPECT_EQ(d.periodUs, 100000U);
        const evdev::Output v = evdev::toEvdev(w);
        EXPECT_EQ(v.type, evdev::kFfPeriodic);
        EXPECT_EQ(v.waveform, waveform);
        EXPECT_EQ(v.periodMs, 100);
        EXPECT_EQ(v.magnitude, 16384);
        EXPECT_EQ(v.phase, 0x4000);
    }
}

// The wheel mounted the other way round: the whole wave turns, its offset and its swing (half a turn on); its
// magnitude stays.
TEST(WheelEffect, InvertingAPeriodicEffectTurnsTheWholeWave) {
    const WheelEffect w = toWheelEffect(wave(FfbEffectType::Sine, {0.5F, 0.25F, 100.0F, 90.0F}), true);
    EXPECT_EQ(w.magnitude, 16384);
    EXPECT_EQ(w.offset, -8192);
    EXPECT_EQ(w.phase, 27000);
    EXPECT_EQ(toWheelEffect(wave(FfbEffectType::Square, {0.5F, 0.0F, 100.0F, 270.0F}), true).phase, 9000);
}

// Phases from a game in any number of turns, either way round, land in 0..35999 hundredths of a degree; one a hair
// under a full turn rounds to 0, never to 36000 (out of DirectInput's range).
TEST(WheelEffect, PhasesWrapIntoOneTurn) {
    const auto phaseOf = [](const float degrees) {
        return toWheelEffect(wave(FfbEffectType::Sine, {0.5F, 0.0F, 100.0F, degrees}), false).phase;
    };
    EXPECT_EQ(phaseOf(0.0F), 0);
    EXPECT_EQ(phaseOf(720.5F), 50);
    EXPECT_EQ(phaseOf(-450.0F), 27000);
    EXPECT_EQ(phaseOf(359.9999F), 0);
    EXPECT_EQ(phaseOf(-0.00001F), 0);
    EXPECT_EQ(phaseOf(std::numeric_limits<float>::infinity()), 0);
    EXPECT_LT(phaseOf(1e30F), 36000);
    // The largest phase there is: DirectInput 35999, evdev just under 0x10000.
    WheelEffect last;
    last.type = FfbEffectType::Sine;
    last.phase = 35999;
    EXPECT_EQ(di::toDirectInput(last).phase, 35999U);
    EXPECT_EQ(evdev::toEvdev(last).phase, 65534);
}

// Periods outside 1..65535 ms (0 from a game that gives none, a NaN, a huge number) are clamped, never undefined;
// evdev's own limit is 32767 ms.
TEST(WheelEffect, PeriodsAreClampedToWhatEachApiTakes) {
    const auto periodOf = [](const float ms) {
        return toWheelEffect(wave(FfbEffectType::Square, {0.5F, 0.0F, ms, 0.0F}), false).periodMs;
    };
    EXPECT_EQ(periodOf(0.0F), 1);
    EXPECT_EQ(periodOf(-50.0F), 1);
    EXPECT_EQ(periodOf(0.6F), 1);
    EXPECT_EQ(periodOf(std::numeric_limits<float>::quiet_NaN()), 1);
    EXPECT_EQ(periodOf(1e9F), 65535);
    EXPECT_EQ(periodOf(std::numeric_limits<float>::infinity()), 1);   // not a number of ms: as no period
    WheelEffect longest;
    longest.type = FfbEffectType::Square;
    longest.periodMs = 65535;
    EXPECT_EQ(di::toDirectInput(longest).periodUs, 65535000U);
    EXPECT_EQ(evdev::toEvdev(longest).periodMs, 32767);
    longest.periodMs = 32767;
    EXPECT_EQ(evdev::toEvdev(longest).periodMs, 32767);
}

// Full scale either way, also the int16 minimum no conversion makes, stays within DirectInput's -10000..10000 and
// never overflows (the magnitude's absolute value included).
TEST(WheelEffect, ExtremesStayInDirectInputsRange) {
    EXPECT_EQ(di::toDirectInput(toWheelEffect(constant(-1.0F), false)).magnitude, -10000);
    EXPECT_EQ(di::toDirectInput(toWheelEffect(constant(5.0F), true)).magnitude, -10000);
    WheelEffect floor;
    floor.type = FfbEffectType::Constant;
    floor.level = std::numeric_limits<std::int16_t>::min();
    EXPECT_EQ(di::toDirectInput(floor).magnitude, -10000);
    floor.type = FfbEffectType::Triangle;
    floor.magnitude = std::numeric_limits<std::int16_t>::min();
    floor.offset = std::numeric_limits<std::int16_t>::min();
    const di::Output p = di::toDirectInput(floor);
    EXPECT_EQ(p.periodicMagnitude, 10000U);
    EXPECT_EQ(p.periodicOffset, -10000);
    EXPECT_EQ(p.phase, 18000U);   // a negative magnitude: half a turn on
    floor.type = FfbEffectType::Spring;
    floor.rightCoefficient = std::numeric_limits<std::int16_t>::min();
    floor.rightSaturation = 65535;
    floor.deadband = 65535;
    const di::Output c = di::toDirectInput(floor);
    EXPECT_EQ(c.positiveCoefficient, -10000);
    EXPECT_EQ(c.positiveSaturation, 10000U);
    EXPECT_EQ(c.deadBand, 10000);
}

// A condition's values out of their ranges or not numbers: coefficients and centre to -1..1, saturations and dead band
// to 0..1, NaN to 0 (no force).
TEST(WheelEffect, ConditionValuesOutOfRangeAreClamped) {
    FfbEffect e = spring();
    e.condition.positiveCoefficient = 3.0F;
    e.condition.negativeCoefficient = std::numeric_limits<float>::quiet_NaN();
    e.condition.positiveSaturation = -1.0F;
    e.condition.negativeSaturation = 9.0F;
    e.condition.deadband = std::numeric_limits<float>::infinity();
    e.condition.center = -4.0F;
    const WheelEffect w = toWheelEffect(e, false);
    EXPECT_EQ(w.rightCoefficient, 32767);
    EXPECT_EQ(w.leftCoefficient, 0);
    EXPECT_EQ(w.rightSaturation, 0);
    EXPECT_EQ(w.leftSaturation, 65535);
    EXPECT_EQ(w.deadband, 0);
    EXPECT_EQ(w.center, -32767);
}

// An effect type this build does not know comes out as no force in both APIs.
TEST(WheelEffect, AnUnknownTypeGivesNoForce) {
    FfbEffect e = constant(0.9F);
    e.type = static_cast<FfbEffectType>(99);
    e.periodic.magnitude = 0.9F;
    const WheelEffect w = toWheelEffect(e, false);
    WheelEffect none;
    none.type = static_cast<FfbEffectType>(99);
    EXPECT_EQ(w, none);
    EXPECT_EQ(di::toDirectInput(w), di::Output{});
    EXPECT_EQ(evdev::toEvdev(w), evdev::Output{});
}

// Hat codes as odd devices declare them: only the Y half (a hat that never goes sideways), a unit X with a wider
// plain Y, a pair with a flat (analogue: two axes), a lone Y with fuzz (analogue: one axis). SDL's rules decide.
TEST(EvdevLayout, HatPairsWithOneHalfOrMixedRanges) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    for (const int code : {0x11, 0x12, 0x13, 0x14, 0x15, 0x17}) caps.axes.set(static_cast<std::size_t>(code));
    caps.absInfo[0x11] = {0, -1, 1, 0, 0, 0};       // HAT0Y only
    caps.absInfo[0x12] = {0, -1, 1, 0, 0, 0};       // HAT1X unit ...
    caps.absInfo[0x13] = {0, -5, 5, 0, 0, 0};       // ... HAT1Y plain but wider: still a hat
    caps.absInfo[0x14] = {0, -100, 100, 0, 4, 0};   // HAT2X with a flat ...
    caps.absInfo[0x15] = {0, -1, 1, 0, 0, 0};       // ... so the pair is two axes
    caps.absInfo[0x17] = {0, -127, 127, 2, 0, 0};   // HAT3Y alone, with fuzz: an axis
    evdev::Layout layout(caps);
    EXPECT_EQ(layout.hats(), 2);
    EXPECT_EQ(layout.axes(), 3);
    RawInputState state;
    layout.fill(state);
    EXPECT_EQ(state.hat, kHatCentered);
    layout.apply(evdev::kEvAbs, 0x11, 1);    // the first hat (HAT0) down
    layout.apply(evdev::kEvAbs, 0x14, 100);  // HAT2X, the first axis: full
    layout.fill(state);
    EXPECT_EQ(state.hat, 4);
    EXPECT_FLOAT_EQ(state.axes[0], 1.0F);
    layout.apply(evdev::kEvAbs, 0x11, -1);
    layout.fill(state);
    EXPECT_EQ(state.hat, 0);                 // up
}

// Events for codes the device never declared, or outside the code ranges (a bad event from a driver): ignored.
TEST(EvdevLayout, EventsForUnknownCodesAreIgnored) {
    const evdev::Capabilities caps = wheelCaps();
    evdev::Layout layout(caps);
    RawInputState before;
    layout.fill(before);
    EXPECT_FALSE(layout.apply(evdev::kEvKey, -1, 1));
    EXPECT_FALSE(layout.apply(evdev::kEvKey, evdev::kKeyCount, 1));
    EXPECT_FALSE(layout.apply(evdev::kEvKey, 0x130, 1));   // a key it does not have
    EXPECT_FALSE(layout.apply(evdev::kEvKey, 30, 1));      // KEY_A: not one of its buttons
    EXPECT_FALSE(layout.apply(evdev::kEvAbs, -1, 5));
    EXPECT_FALSE(layout.apply(evdev::kEvAbs, evdev::kAbsCount, 5));
    EXPECT_FALSE(layout.apply(evdev::kEvAbs, 0x01, 5));    // Y: not declared
    EXPECT_FALSE(layout.apply(evdev::kEvSyn, 0, 0));       // SYN_REPORT is not a drop
    EXPECT_FALSE(layout.apply(0x02, 0x00, 5));             // EV_REL
    EXPECT_FALSE(layout.apply(0x7F, 0, 0));
    RawInputState after;
    layout.fill(after);
    EXPECT_EQ(before, after);
    EXPECT_EQ(layout.axisValue(-1), 0);
    EXPECT_EQ(layout.axisValue(3), 0);
}

// [emulator] source_device written oddly: in another case, part of the id across its colon, a product id alone, an
// empty match (any device), or a name that is not there.
TEST(DeviceMatch, OddMatchesByNameAndId) {
    EXPECT_TRUE(deviceMatches("MOZA R3 Base", 0x346e, 0x0005, "moza r3"));
    EXPECT_TRUE(deviceMatches("", 0x346e, 0x0005, "6E:00"));     // across the colon: still part of "346e:0005"
    EXPECT_TRUE(deviceMatches("Wheel", 0x346e, 0x0005, "0005"));
    EXPECT_TRUE(deviceMatches("Wheel", 0x046d, 0xc262, ""));      // an empty match is any device
    EXPECT_FALSE(deviceMatches("Wheel", 0x346e, 0x0005, "346e:0006"));
    EXPECT_FALSE(deviceMatches("Wheel", 0x346e, 0x0005, "346e:00050"));
    EXPECT_FALSE(deviceMatches("", 0, 0, "wheel"));
    EXPECT_TRUE(deviceMatches("", 0, 0, "0000:0000"));
    EXPECT_TRUE(deviceMatches("Pedals \xC3\x84", 0xffff, 0xffff, "ffff:ffff"));   // UTF-8 in the name: no harm
}


// More of SDL's hat rules: a hat axis from -1 to more than 1 is not "unit" but still plain (a hat); one that
// declares a resolution is analogue.
TEST(EvdevLayout, HatRangesAndResolutionDecideDigital) {
    evdev::Capabilities caps;
    caps.keys.set(0x120);
    for (const int code : {0x10, 0x12}) caps.axes.set(static_cast<std::size_t>(code));
    caps.absInfo[0x10] = {0, -1, 5, 0, 0, 0};    // HAT0X: not -1..1, but plain: a hat
    caps.absInfo[0x12] = {0, -1, 1, 0, 0, 1};    // HAT1X: a resolution: an axis
    caps.axes.set(0x13);
    caps.absInfo[0x13] = {0, -8, 8, 0, 0, 0};
    evdev::Layout layout(caps);
    EXPECT_EQ(layout.hats(), 1);
    EXPECT_EQ(layout.axes(), 2);
    layout.apply(evdev::kEvAbs, 0x10, 5);
    RawInputState state;
    layout.fill(state);
    EXPECT_EQ(state.hat, 2);   // right
}

}  // namespace
}  // namespace wheelbri
