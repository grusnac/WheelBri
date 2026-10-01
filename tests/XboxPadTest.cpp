#include "wheelemu/XboxPad.hpp"

#include <gtest/gtest.h>

namespace wheelemu {
namespace {

TargetReport released() {
    TargetReport r;
    r.steering = 32768;
    r.throttle = 65535;
    r.brake = 65535;
    r.clutch = 65535;
    r.buttons = 0;
    r.hat = -1;
    return r;
}

TEST(XusbPacket, RoundTripAndRejects) {
    XusbPad pad;
    pad.buttons = XusbPad::kA | XusbPad::kDpadUp;
    pad.leftTrigger = 7;
    pad.thumbLX = -1234;
    pad.thumbRY = 32767;
    const auto bytes = encodeXusbPacket(pad);
    const auto back = decodeXusbPacket(bytes);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, pad);
    auto broken = bytes;
    broken[0] ^= 0xFF;
    EXPECT_FALSE(decodeXusbPacket(broken).has_value());
    EXPECT_FALSE(decodeXusbPacket(std::span(bytes).first(10)).has_value());
}

TEST(XboxMirror, ButtonsGoToThePadInMenus) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    r.buttons = 1U << 0;   // A
    auto out = m.update(r, 10'000, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, XusbPad::kA);
    EXPECT_TRUE(out.firstPress);
    EXPECT_EQ(out.g920.buttons, r.buttons);   // the G920 keeps its buttons
    r.buttons = (1U << 6) | (1U << 4) | (1U << 10);   // Menu, a paddle, the Xbox button
    out = m.update(r, 10'010, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, XusbPad::kStart);      // paddles and Xbox are never passed on
}

TEST(XboxMirror, NothingWhileDrivingAndHeldButtonsMustBeReleased) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    r.throttle = 0;                 // full throttle
    r.buttons = 1U << 1;            // B held while driving
    EXPECT_EQ(m.update(r, 1000, 0.5F, true).pad.buttons, 0);
    r.throttle = 65535;
    EXPECT_EQ(m.update(r, 2500, 0.5F, true).pad.buttons, 0);   // 1.5 s after the pedal: still driving
    EXPECT_EQ(m.update(r, 3100, 0.5F, true).pad.buttons, 0);   // quiet now, but B was held since driving
    r.buttons = 0;
    (void)m.update(r, 3200, 0.5F, true);
    r.buttons = 1U << 1;
    EXPECT_EQ(m.update(r, 3300, 0.5F, true).pad.buttons, XusbPad::kB);   // a fresh press
}

TEST(XboxMirror, DpadGoesToThePadInsteadOfTheG920) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    r.hat = 2;   // right
    const auto out = m.update(r, 10'000, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, XusbPad::kDpadRight);
    EXPECT_EQ(out.g920.hat, -1);
    r.hat = 7;
    EXPECT_EQ(m.update(r, 10'010, 0.0F, false).pad.buttons, XusbPad::kDpadUp | XusbPad::kDpadLeft);
}

TEST(XboxMirror, MenuAndViewGoToThePadInsteadOfTheG920) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    r.buttons = (1U << 6) | (1U << 0);   // Menu and A
    auto out = m.update(r, 10'000, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, XusbPad::kStart | XusbPad::kA);
    EXPECT_EQ(out.g920.buttons, 1U << 0);   // A stays on the G920, Menu does not
    r.buttons = 1U << 7;                    // View
    out = m.update(r, 10'010, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, XusbPad::kBack);
    EXPECT_EQ(out.g920.buttons, 0U);
    r.throttle = 0;                         // driving off while View is still held: no View on the G920
    out = m.update(r, 10'020, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, 0);
    EXPECT_EQ(out.g920.buttons, 0U);
    r.buttons = 0;
    (void)m.update(r, 10'030, 0.0F, false);
    r.buttons = 1U << 6;                    // Menu while driving: the G920's (the in-race pause)
    out = m.update(r, 10'040, 0.0F, false);
    EXPECT_EQ(out.pad.buttons, 0);
    EXPECT_EQ(out.g920.buttons, 1U << 6);
}

TEST(XboxMirror, SteeringHeldOnlyInARealMenu) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    r.steering = 40000;
    auto out = m.update(r, 0, 0.6F, true);           // force playing: driving
    EXPECT_EQ(out.g920.steering, 40000);
    out = m.update(r, 1000, 0.0F, true);             // forces at zero for 1 s: not yet
    EXPECT_FALSE(m.inMenu());
    out = m.update(r, 2600, 0.0F, true);             // 1.6 s at zero, no pedal: a menu
    ASSERT_TRUE(out.menuChanged.has_value());
    EXPECT_TRUE(*out.menuChanged);
    r.steering = 10000;
    EXPECT_EQ(m.update(r, 2700, 0.0F, true).g920.steering, 40000);   // held where it was
    out = m.update(r, 2800, 0.4F, true);             // a force again: given back at once
    EXPECT_EQ(out.g920.steering, 10000);
    ASSERT_TRUE(out.menuChanged.has_value());
    EXPECT_FALSE(*out.menuChanged);
}

TEST(XboxMirror, NeverHeldWithoutForceFeedbackOrInIdleMode) {
    XboxMirror noFfb(XboxPadMode::Buttons);
    TargetReport r = released();
    r.steering = 50000;
    EXPECT_EQ(noFfb.update(r, 100'000, 0.0F, false).g920.steering, 50000);
    EXPECT_FALSE(noFfb.inMenu());

    XboxMirror idle(XboxPadMode::Idle);
    r.buttons = 1U << 0;
    r.hat = 0;
    const auto out = idle.update(r, 100'000, 0.0F, true);
    EXPECT_EQ(out.pad.buttons, 0);
    EXPECT_EQ(out.g920.hat, 0);
}

}  // namespace
}  // namespace wheelemu
