#include "wheelbri/HidPid.hpp"
#include "wheelbri/HidPp.hpp"
#include "wheelbri/HidPpFfb.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

struct Ffb {
    std::vector<FfbPacket> packets;
    std::vector<std::string> lines;
    HidPpFfb ffb{[this](const FfbPacket& p) { packets.push_back(p); },
                 [this](LogLevel, const std::string& line) { lines.push_back(line); }};

    // The last packet for service slot `slot`.
    const FfbPacket* last(const std::uint8_t slot) const {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->kind == FfbPacket::Kind::SlotState && it->slot == slot) return &*it;
        }
        return nullptr;
    }
};

// A download's parameters: slot, type, duration ms, delay ms, then the type's own bytes.
std::vector<std::uint8_t> effect(const std::uint8_t slot, const std::uint8_t type, const std::uint16_t durationMs,
                                 const std::uint16_t delayMs, const std::vector<std::uint8_t>& rest) {
    std::vector<std::uint8_t> p{slot,
                                type,
                                static_cast<std::uint8_t>(durationMs >> 8),
                                static_cast<std::uint8_t>(durationMs & 0xFF),
                                static_cast<std::uint8_t>(delayMs >> 8),
                                static_cast<std::uint8_t>(delayMs & 0xFF)};
    p.insert(p.end(), rest.begin(), rest.end());
    p.resize(18, 0);
    return p;
}

TEST(HidPpFfb, ConstantForceStartsAtOnceAndAnUpdateKeepsItPlaying) {
    Ffb f;
    EXPECT_EQ(f.ffb.download(effect(0, 0x80, 0, 0, {0x40, 0x00}), 0), 1);   // autostart, level 0x4000
    const FfbPacket* p = f.last(0);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->playing);
    EXPECT_EQ(p->effect.type, FfbEffectType::Constant);
    EXPECT_NEAR(p->effect.constantLevel, 0.5F, 0.001F);
    // The game moves its force: the same slot again, no autostart.
    EXPECT_EQ(f.ffb.download(effect(1, 0x00, 0, 0, {0xC0, 0x00}), 16), 1);
    p = f.last(0);
    EXPECT_TRUE(p->playing);
    EXPECT_NEAR(p->effect.constantLevel, -0.5F, 0.001F);
}

TEST(HidPpFfb, StatePlaysAndStopsAndFreesTheServiceSlot) {
    Ffb f;
    const std::uint8_t slot = f.ffb.download(effect(0, 0x00, 0, 0, {0x20, 0x00}), 0);
    EXPECT_EQ(f.last(0), nullptr);   // loaded, not playing
    f.ffb.setState(slot, 2, 1);
    ASSERT_NE(f.last(0), nullptr);
    EXPECT_TRUE(f.last(0)->playing);
    f.ffb.setState(slot, 1, 2);
    EXPECT_FALSE(f.last(0)->playing);
    // Another effect takes service slot 0 again.
    const std::uint8_t other = f.ffb.download(effect(0, 0x80, 0, 0, {0x10, 0x00}), 3);
    EXPECT_NE(other, slot);
    EXPECT_TRUE(f.last(0)->playing);
    EXPECT_NEAR(f.last(0)->effect.constantLevel, 0.125F, 0.001F);
}

TEST(HidPpFfb, ConditionAndPeriodicDecode) {
    Ffb f;
    // Spring: left saturation 1, left k 0.5, deadband 0.1, centre 0, right k 0.25, right saturation 0.5.
    const std::vector<std::uint8_t> spring{0x7F, 0xFF, 0x40, 0x00, 0x0C, 0xCC, 0x00, 0x00, 0x20, 0x00, 0x3F, 0xFF};
    (void)f.ffb.download(effect(0, 0x86, 0, 0, spring), 0);
    const FfbPacket* played = f.last(0);
    ASSERT_NE(played, nullptr);
    EXPECT_EQ(played->effect.type, FfbEffectType::Spring);
    EXPECT_NEAR(played->effect.condition.negativeSaturation, 1.0F, 0.001F);
    EXPECT_NEAR(played->effect.condition.negativeCoefficient, 0.5F, 0.001F);
    EXPECT_NEAR(played->effect.condition.deadband, 0.1F, 0.001F);
    EXPECT_NEAR(played->effect.condition.positiveCoefficient, 0.25F, 0.001F);
    EXPECT_NEAR(played->effect.condition.positiveSaturation, 0.5F, 0.001F);
    // Sine: magnitude -0.5 (as 0.5, half a turn later), period 100 ms, phase 90 degrees.
    (void)f.ffb.download(effect(0, 0x81, 0, 0, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x64, 0x40, 0x00}), 1);
    const FfbPacket* sine = f.last(1);
    ASSERT_NE(sine, nullptr);
    EXPECT_EQ(sine->effect.type, FfbEffectType::Sine);
    EXPECT_NEAR(sine->effect.periodic.magnitude, 0.5F, 0.001F);
    EXPECT_NEAR(sine->effect.periodic.periodMs, 100.0F, 0.001F);
    EXPECT_NEAR(sine->effect.periodic.phaseDeg, 270.0F, 0.01F);
    EXPECT_EQ(f.ffb.download(effect(0, 0x7E, 0, 0, {}), 2), 0);   // a type it does not know
    EXPECT_EQ(f.ffb.download(effect(64, 0x00, 0, 0, {}), 2), 0);   // a slot out of range
}

TEST(HidPpFfb, DelayAndDurationAreKept) {
    Ffb f;
    (void)f.ffb.download(effect(0, 0x80, 50, 20, {0x40, 0x00}), 0);
    EXPECT_FALSE(f.last(0)->playing);   // waiting for its delay
    f.ffb.tick(25);
    EXPECT_TRUE(f.last(0)->playing);
    f.ffb.tick(60);
    EXPECT_TRUE(f.last(0)->playing);
    f.ffb.tick(75);                      // 20 + 50 ms: over
    EXPECT_FALSE(f.last(0)->playing);
}

TEST(HidPpFfb, GlobalGainScalesEveryEffect) {
    Ffb f;
    (void)f.ffb.download(effect(0, 0x80, 0, 0, {0x40, 0x00}), 0);
    f.ffb.setGain(0.5F, 1);
    EXPECT_NEAR(f.last(0)->effect.gain, 0.5F, 0.001F);
}

TEST(HidPpFfb, OnlyAsManyEffectsPlayAsTheServiceHasSlots) {
    Ffb f;
    for (std::size_t i = 0; i <= HidPpFfb::kServiceSlots; ++i) (void)f.ffb.download(effect(0, 0x80, 0, 0, {1}), 0);
    for (std::uint8_t s = 0; s < HidPpFfb::kServiceSlots; ++s) EXPECT_TRUE(f.last(s)->playing);
    EXPECT_EQ(f.last(HidPpFfb::kServiceSlots), nullptr);
    EXPECT_EQ(std::count_if(f.lines.begin(), f.lines.end(),
                            [](const std::string& l) { return l.find("not played") != std::string::npos; }),
              1);
}

TEST(HidPpFfb, ResetStopsEverything) {
    Ffb f;
    (void)f.ffb.download(effect(0, 0x80, 0, 0, {0x40, 0x00}), 0);
    f.ffb.resetAll(1);
    EXPECT_EQ(f.packets.back().kind, FfbPacket::Kind::StopAll);
    EXPECT_EQ(f.ffb.download(effect(0, 0x00, 0, 0, {}), 2), 1);   // every slot free again
}

// Logitech's driver resets at every game start: with nothing playing, the rig gets no stop-all.
TEST(HidPpFfb, ResetWithNothingPlayingSendsNothing) {
    Ffb f;
    f.ffb.resetAll(1);
    EXPECT_TRUE(f.packets.empty());
}

TEST(HidPpFfb, KeepAlivesOnlyOnceAnEffectCame) {
    Ffb f;
    f.ffb.setGain(1.0F, 0);   // G HUB at start: not a game
    f.ffb.tick(500);
    EXPECT_TRUE(f.packets.empty());
    (void)f.ffb.download(effect(0, 0x00, 0, 0, {}), 600);
    f.ffb.tick(700);
    ASSERT_FALSE(f.packets.empty());
    EXPECT_EQ(f.packets.back().kind, FfbPacket::Kind::KeepAlive);
}

// A game that streams its force (every 16 ms) and then goes silent while a force pushes has closed or crashed.
TEST(HidPpFfb, AStreamThatStopsWhilePushingStopsTheKeepAlives) {
    Ffb f;
    std::int64_t t = 0;
    for (; t <= 2500; t += 16) {
        (void)f.ffb.download(effect(1, 0x80, 0, 0, {0x30, 0x00}), t);
        f.ffb.tick(t);
    }
    const std::size_t before = f.packets.size();
    for (std::int64_t later = t; later < t + 2000; later += 10) f.ffb.tick(later);
    const auto keepAlives = std::count_if(f.packets.begin() + static_cast<std::ptrdiff_t>(before), f.packets.end(),
                                          [](const FfbPacket& p) { return p.kind == FfbPacket::Kind::KeepAlive; });
    EXPECT_LE(keepAlives, 5);   // at most until the 500 ms rule
    EXPECT_NE(std::find_if(f.lines.begin(), f.lines.end(),
                           [](const std::string& l) { return l.find("keep-alives stopped") != std::string::npos; }),
              f.lines.end());
}

// A game that sends nothing for 10 s while even a small force (1.5%) pushes has gone: keep-alives stop, and the
// service's watchdog stops the force.
TEST(HidPpFfb, ASilentGameWithASmallPushStopsTheKeepAlives) {
    Ffb f;
    (void)f.ffb.download(effect(0, 0x80, 0, 0, {0x01, 0xEC}), 0);   // autostart, level 0x01EC: 1.5%
    for (std::int64_t t = 10; t < HidPid::kGameQuietMs; t += 10) f.ffb.tick(t);
    const std::size_t before = f.packets.size();
    for (std::int64_t t = HidPid::kGameQuietMs; t < HidPid::kGameQuietMs + 2000; t += 10) f.ffb.tick(t);
    EXPECT_EQ(f.packets.size(), before);
}

// HidPp hands 0x8123 to its HidPpFfb: download answers with the slot, gains reach every effect.
TEST(HidPpFfb, HidPpPassesItsEffectsOn) {
    Ffb f;
    HidPp hidpp({"G923", 0xC26E, 1, 0x0A}, [](LogLevel, const std::string&) {});
    hidpp.setForceFeedback(&f.ffb);
    std::vector<std::uint8_t> request(HidPp::kLongSize, 0);
    request[0] = HidPp::kLongId;
    request[1] = 0xFF;
    request[2] = 4;   // ForceFeedback's index
    request[3] = 0x21;
    const auto params = effect(0, 0x80, 0, 0, {0x40, 0x00});
    std::copy(params.begin(), params.begin() + 16, request.begin() + 4);
    const auto answer = hidpp.handle(request, 5);
    ASSERT_TRUE(answer);
    EXPECT_EQ((*answer)[2], 4);   // not an error
    EXPECT_EQ((*answer)[4], 1);   // slot 1
    ASSERT_NE(f.last(0), nullptr);
    EXPECT_TRUE(f.last(0)->playing);
    std::fill(request.begin() + 3, request.end(), 0);
    request[3] = 0x81;   // set global gains: half
    request[4] = 0x7F;
    request[5] = 0xFF;
    (void)hidpp.handle(request, 6);
    EXPECT_NEAR(f.last(0)->effect.gain, 0.5F, 0.001F);
}

}  // namespace
}  // namespace wheelbri
