// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/ClassicFfb.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

using Report = std::vector<std::uint8_t>;

struct Ffb {
    std::vector<FfbPacket> packets;
    std::vector<std::string> lines;
    ClassicFfb ffb{[this](const FfbPacket& p) { packets.push_back(p); },
                   [this](LogLevel, const std::string& line) { lines.push_back(line); }};

    void send(const Report& r, const std::int64_t nowMs = 0) { ffb.command(r, nowMs); }
    // The last state sent for service slot `slot`.
    const FfbPacket* last(const std::uint8_t slot) const {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->kind == FfbPacket::Kind::SlotState && it->slot == slot) return &*it;
        }
        return nullptr;
    }
    [[nodiscard]] int linesWith(const std::string& part) const {
        return static_cast<int>(std::count_if(lines.begin(), lines.end(),
                                              [&](const std::string& l) { return l.find(part) != std::string::npos; }));
    }
};

// Table 22: 0 the full force towards the axis' 0 end (left, -1), 255 towards its 255 end (right, +1), 127 and 128
// next to none (checked on a Moza R3 in ACC: the other way round pushed the wheel away from the centre).
TEST(ClassicFfb, LevelsTowardsTheirEndOfTheAxis) {
    EXPECT_FLOAT_EQ(ClassicFfb::level(0), -1.0F);
    EXPECT_FLOAT_EQ(ClassicFfb::level(255), 1.0F);
    EXPECT_NEAR(ClassicFfb::level(127), 0.0F, 0.005F);
    EXPECT_NEAR(ClassicFfb::level(128), 0.0F, 0.005F);
    EXPECT_LT(ClassicFfb::level(0x40), 0.0F);
    EXPECT_GT(ClassicFfb::level(0xC0), 0.0F);
}

// ACC's steering force as Logitech's driver sent it (2026-11-03): slot F0, download and play, type 0x08 (variable),
// F0's level in byte 2 (F2's 0x80: none). It plays at once; every new level is an update of the same slot.
TEST(ClassicFfb, AccsSteeringForceIsAVariableForceInSlotZero) {
    Ffb f;
    f.send({0x11, 0x08, 0x79, 0x80, 0x00, 0x00, 0x00});
    const FfbPacket* p = f.last(0);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->playing);
    EXPECT_EQ(p->effect.type, FfbEffectType::Constant);
    EXPECT_NEAR(p->effect.constantLevel, ClassicFfb::level(0x79), 1e-6F);
    EXPECT_LT(p->effect.constantLevel, 0.0F);
    f.send({0x11, 0x08, 0x87, 0x80, 0x00, 0x00, 0x00}, 4);
    p = f.last(0);
    EXPECT_TRUE(p->playing);
    EXPECT_GT(p->effect.constantLevel, 0.0F);
    EXPECT_EQ(f.last(2), nullptr);   // F2 not selected: nothing there
}

// ACC's damper: slot F1, high-resolution damper (0x0c), K1 left, S1, K2 right, S2; byte 6 (a clip on a Driving Force
// Pro only) ignored.
TEST(ClassicFfb, AccsDamperIsAHighResolutionDamperInSlotOne) {
    Ffb f;
    f.send({0x21, 0x0C, 0x0A, 0x00, 0x0F, 0x00, 0x01});
    const FfbPacket* p = f.last(1);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->playing);
    EXPECT_EQ(p->effect.type, FfbEffectType::Damper);
    EXPECT_NEAR(p->effect.condition.negativeCoefficient, 10.0F / 15.0F, 1e-6F);
    EXPECT_NEAR(p->effect.condition.positiveCoefficient, 1.0F, 1e-6F);
    EXPECT_FLOAT_EQ(p->effect.condition.positiveSaturation, 1.0F);
    // S1 set: the left side accentuates the movement instead.
    f.send({0x21, 0x0C, 0x0A, 0x01, 0x0F, 0x00, 0x00});
    EXPECT_NEAR(f.last(1)->effect.condition.negativeCoefficient, -10.0F / 15.0F, 1e-6F);
}

// Stop one slot (0x13: F0), stop all (0xf3); download alone (0x00) loads without playing; play (0x02) starts it;
// refresh (0x0c) changes a playing one.
TEST(ClassicFfb, DownloadPlayRefreshAndStop) {
    Ffb f;
    f.send({0x31, 0x08, 0x40, 0x80, 0, 0, 0});   // F0 and F1 (F1 is not a variable force's slot: nothing there)
    ASSERT_NE(f.last(0), nullptr);
    EXPECT_EQ(f.last(1), nullptr);
    f.send({0x21, 0x02, 0x03, 0x00, 0x03, 0x00, 0x00});   // F1 damper, playing
    f.send({0x13, 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(f.last(0)->playing);
    EXPECT_TRUE(f.last(1)->playing);
    f.send({0xF3, 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(f.last(1)->playing);

    Ffb g;
    g.send({0x40, 0x00, 0x80, 0x80, 0x20, 0x80, 0x00});   // F2 constant, downloaded only
    ASSERT_NE(g.last(2), nullptr);
    EXPECT_FALSE(g.last(2)->playing);
    g.send({0x42, 0, 0, 0, 0, 0, 0});                     // play F2
    EXPECT_TRUE(g.last(2)->playing);
    EXPECT_NEAR(g.last(2)->effect.constantLevel, ClassicFfb::level(0x20), 1e-6F);
    g.send({0x4C, 0x00, 0x80, 0x80, 0xE0, 0x80, 0x00});   // refresh F2
    EXPECT_TRUE(g.last(2)->playing);
    EXPECT_NEAR(g.last(2)->effect.constantLevel, ClassicFfb::level(0xE0), 1e-6F);
    g.send({0x82, 0, 0, 0, 0, 0, 0});                     // play F3: nothing loaded there
    EXPECT_EQ(g.last(3), nullptr);
}

// A constant force (0x00) has one level per selected slot, bytes 2..5.
TEST(ClassicFfb, AConstantForceHasALevelPerSlot) {
    Ffb f;
    f.send({0x91, 0x00, 0x00, 0x80, 0x80, 0xFF, 0x00});   // F0 and F3
    EXPECT_FLOAT_EQ(f.last(0)->effect.constantLevel, -1.0F);
    EXPECT_FLOAT_EQ(f.last(3)->effect.constantLevel, 1.0F);
    EXPECT_EQ(f.last(1), nullptr);
}

// Springs: dead band D1..D2 (centre and half-width), K1 left / K2 right (3-bit table as a quarter of the factor;
// high-resolution 0..15 linear), S1/S2 inverting a side, CLIP the saturation of both.
TEST(ClassicFfb, SpringsLowAndHighResolution) {
    std::array<std::uint8_t, ClassicFfb::kReportSize> r{0x11, 0x01, 0x70, 0x90, 0x53, 0x10, 0xFF};
    auto e = ClassicFfb::decode(r, 0);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->type, FfbEffectType::Spring);
    EXPECT_FLOAT_EQ(e->condition.negativeCoefficient, 1.0F / 4.0F);    // K1 3: the offset itself
    EXPECT_FLOAT_EQ(e->condition.positiveCoefficient, -2.0F / 4.0F);   // K2 5: twice, S2 inverted
    EXPECT_FLOAT_EQ(e->condition.positiveSaturation, 1.0F);
    EXPECT_NEAR(e->condition.center, 0.0F, 0.01F);
    EXPECT_NEAR(e->condition.deadband, 32.0F / 255.0F, 1e-6F);
    r = {0x11, 0x0B, 0x00, 0x00, 0xF0, 0x00, 0x80};
    e = ClassicFfb::decode(r, 0);
    EXPECT_FLOAT_EQ(e->condition.negativeCoefficient, 0.0F);
    EXPECT_FLOAT_EQ(e->condition.positiveCoefficient, 1.0F);   // K2 15
    EXPECT_NEAR(e->condition.positiveSaturation, 128.0F / 255.0F, 1e-6F);
    EXPECT_NEAR(e->condition.center, -1.0F, 1e-6F);
    r = {0x11, 0x0D, 0x0F, 0x0F, 0xFF, 0x00, 0x00};   // high-resolution auto-centring: centred
    e = ClassicFfb::decode(r, 0);
    EXPECT_EQ(e->type, FfbEffectType::Spring);
    EXPECT_FLOAT_EQ(e->condition.center, 0.0F);
    EXPECT_FLOAT_EQ(e->condition.negativeCoefficient, 1.0F);
    r = {0x11, 0x0E, 0xFF, 0x80, 0xFF, 0x10, 0x00};   // friction
    e = ClassicFfb::decode(r, 0);
    EXPECT_EQ(e->type, FfbEffectType::Friction);
    EXPECT_FLOAT_EQ(e->condition.negativeCoefficient, 1.0F);
    EXPECT_NEAR(e->condition.positiveCoefficient, -128.0F / 255.0F, 1e-6F);
}

// Periodic forces and ramps are not played yet, default springs and the rest are only logged: each said once.
TEST(ClassicFfb, WhatIsNotPlayedIsLoggedOnce) {
    Ffb f;
    for (int i = 0; i < 3; ++i) f.send({0x11, 0x04, 0xFF, 0x00, 0x80, 0x00, 0x11});
    EXPECT_EQ(f.last(0), nullptr);
    EXPECT_EQ(f.linesWith("sawtooth up"), 1);
    for (int i = 0; i < 3; ++i) f.send({0x14, 0, 0, 0, 0, 0, 0});
    f.send({0xF5, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(f.linesWith("default spring"), 1);
    EXPECT_TRUE(f.packets.empty());
    f.send({0xF8, 0x81, 0x84, 0x03, 0, 0, 0});   // wheel range 900
    EXPECT_EQ(f.linesWith("900 deg"), 1);
}

// Keep-alives once an effect came (none before), and a stop-all at the end only then.
TEST(ClassicFfb, KeepAlivesOnlyOnceAnEffectCame) {
    Ffb f;
    f.ffb.tick(500);
    EXPECT_TRUE(f.packets.empty());
    f.send({0x11, 0x08, 0x70, 0x80, 0, 0, 0}, 1000);
    f.ffb.tick(1200);   // a refresh of the playing force
    f.ffb.tick(1310);   // nothing new for over kKeepAliveMs: a keep-alive
    EXPECT_TRUE(std::any_of(f.packets.begin(), f.packets.end(),
                            [](const FfbPacket& p) { return p.kind == FfbPacket::Kind::KeepAlive; }));
    f.ffb.stopAll(1400);
    EXPECT_EQ(f.packets.back().kind, FfbPacket::Kind::StopAll);
    Ffb g;
    g.ffb.stopAll(0);
    EXPECT_TRUE(g.packets.empty());
}

// Short and long reports: padded and cut, never read past their end.
TEST(ClassicFfb, ShortAndLongReports) {
    Ffb f;
    f.send({0x11, 0x08});
    ASSERT_NE(f.last(0), nullptr);
    EXPECT_FLOAT_EQ(f.last(0)->effect.constantLevel, -1.0F);   // level byte 0: missing is zero
    f.send({0x11, 0x08, 0x80, 0x80, 0, 0, 0, 0xFF, 0xFF});
    EXPECT_NEAR(f.last(0)->effect.constantLevel, 0.0F, 0.005F);
    f.send({});
}

}  // namespace
}  // namespace wheelbri
