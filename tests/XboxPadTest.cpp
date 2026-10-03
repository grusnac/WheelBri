#include "wheelbri/XboxPad.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace wheelbri {
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

// Steps the mirror through `ms` milliseconds of the same wheel state; the last result.
XboxMirror::Result run(XboxMirror& m, const TargetReport& r, std::int64_t& now, const std::int64_t ms) {
    XboxMirror::Result out;
    for (std::int64_t end = now + ms; now < end; ++now) out = m.update(r, now);
    return out;
}

TEST(XboxMirror, TheG920GetsEverythingAndThePadOnlyANudge) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    std::int64_t now = 1000;
    (void)run(m, r, now, 500);   // the start: nothing to nudge
    EXPECT_EQ(m.nudges(), 0U);

    r.buttons = (1U << 8) | (1U << 4);   // RSB (the Moza's R) and a paddle
    auto out = m.update(r, now++);
    EXPECT_EQ(out.g920.buttons, r.buttons);   // to the G920, untouched
    EXPECT_EQ(out.nudged, XboxMirror::Cause::Button);
    EXPECT_EQ(out.pad.buttons, 0);            // the pad never presses anything
    EXPECT_EQ(out.pad.leftTrigger, 0);
    EXPECT_EQ(out.pad.rightTrigger, 0);
    EXPECT_EQ(out.pad.thumbLX, 0);            // the left stick (steering) never moves
    EXPECT_EQ(out.pad.thumbLY, 0);
    EXPECT_EQ(out.pad.thumbRX, 0);
    EXPECT_GT(std::abs(out.pad.thumbRY), 7849);   // the nudge: past XInput's dead zone, so the game counts it
    out = run(m, r, now, XboxMirror::kNudgeMs + 5);   // held: one nudge only, and it ends
    EXPECT_EQ(m.nudges(), 1U);
    EXPECT_EQ(out.pad, XusbPad{});
}

TEST(XboxMirror, PedalsAndDpadNudgeTooButNotSteering) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    std::int64_t now = 1000;
    (void)run(m, r, now, 300);

    r.brake = 0;   // pressed
    auto out = m.update(r, now++);
    EXPECT_EQ(out.nudged, XboxMirror::Cause::Pedal);
    EXPECT_EQ(out.g920.brake, 0);   // never held back
    (void)run(m, r, now, 300);
    r.brake = 65535;
    r.clutch = 0;
    EXPECT_EQ(m.update(r, now++).nudged, XboxMirror::Cause::Pedal);
    (void)run(m, r, now, 300);

    r.hat = 2;
    EXPECT_EQ(m.update(r, now++).nudged, XboxMirror::Cause::Dpad);
    (void)run(m, r, now, 300);

    r.steering = 50000;   // the wheel turned: no nudge (it would move the camera while driving)
    out = run(m, r, now, 300);
    EXPECT_EQ(out.g920.steering, 50000);   // never held
    EXPECT_EQ(out.pad, XusbPad{});
    EXPECT_EQ(m.nudges(), 3U);
}

TEST(XboxMirror, NudgesAreSpacedAndAlternate) {
    XboxMirror m(XboxPadMode::Buttons);
    TargetReport r = released();
    std::int64_t now = 1000;
    (void)run(m, r, now, 300);
    r.buttons = 1;
    const auto first = m.update(r, now++);
    r.buttons = 0;
    (void)m.update(r, now++);
    r.buttons = 2;                                   // 2 ms later: waits for kNudgeGapMs
    EXPECT_EQ(m.update(r, now++).nudged, XboxMirror::Cause::None);
    std::int64_t waited = 0;
    XboxMirror::Result second;
    while ((second = m.update(r, now++)).nudged == XboxMirror::Cause::None && waited++ < 1000) {
    }
    EXPECT_EQ(second.nudged, XboxMirror::Cause::Button);   // not lost, only later
    EXPECT_EQ(second.pad.thumbRY, -first.pad.thumbRY);    // the other way: a change even back to back
    EXPECT_EQ(m.nudges(), 2U);
}

TEST(XboxMirror, IdleNeverTouchesThePad) {
    XboxMirror idle(XboxPadMode::Idle);
    TargetReport r = released();
    std::int64_t now = 1000;
    (void)run(idle, r, now, 10);
    r.buttons = 1U << 0;
    r.hat = 0;
    const auto out = idle.update(r, now);
    EXPECT_EQ(out.pad, XusbPad{});
    EXPECT_EQ(out.g920.hat, 0);
    EXPECT_EQ(idle.nudges(), 0U);
}

}  // namespace
}  // namespace wheelbri
