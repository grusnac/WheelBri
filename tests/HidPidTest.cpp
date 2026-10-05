#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidPid.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

#include <string>
#include <vector>

namespace wheelbri {
namespace {

std::vector<std::uint8_t> hex(const std::string& text) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

struct Recorder {
    std::vector<FfbPacket> packets;
    std::vector<std::string> logs;
    HidPid pid{[this](const FfbPacket& p) { packets.push_back(p); }, [this](const std::string& s) { logs.push_back(s); }};

    const FfbPacket* lastPlaying() const {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->kind == FfbPacket::Kind::SlotState && it->playing) return &*it;
        }
        return nullptr;
    }
};

// The reports Windows' pid.dll sent for the old bridge's FfbProbe (2026-09-30): parameters with EBI 0, then
// Set Effect naming the block, then Effect Operation.
TEST(HidPid, PidDllSequenceFromTheProbe) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    std::array<std::uint8_t, 8> feature{};
    ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
    EXPECT_EQ(feature[1], 1);                // EBI 1
    EXPECT_EQ(feature[2], 1);                // success

    r.pid.output(0x15, hex("00A00F"), 0);    // constant 4000, EBI 0
    r.pid.output(0x11, hex("0101DC05000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // Set Effect EBI 1, 90 degrees
    r.pid.output(0x1A, hex("010101"), 1);
    const FfbPacket* p = r.lastPlaying();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->effect.type, FfbEffectType::Constant);
    EXPECT_NEAR(p->effect.constantLevel, 0.4F, 1e-3F);
    EXPECT_NEAR(p->effect.gain, 1.0F, 1e-3F);

    r.pid.output(0x15, hex("00E0FA"), 100);  // -1312 while running: an EBI-0 update alone
    p = r.lastPlaying();
    ASSERT_NE(p, nullptr);
    EXPECT_NEAR(p->effect.constantLevel, -0.1312F, 1e-3F);

    r.pid.output(0x1B, hex("01"), 200);      // block free: the slot stops
    EXPECT_FALSE(r.packets.back().playing);
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0800"), 300);
    ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
    EXPECT_EQ(feature[1], 1);                // freed block reused

    r.pid.output(0x13, hex("000F000070177017102710270000"), 300);   // spring 6000, sat 10000, nibble 0xF
    r.pid.output(0x11, hex("0108B80B000000000000FFFF04FF1F0000FFFFFFFF"), 300);
    r.pid.output(0x1A, hex("010101"), 301);
    p = r.lastPlaying();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->effect.type, FfbEffectType::Spring);
    EXPECT_NEAR(p->effect.condition.positiveCoefficient, 0.6F, 1e-3F);
    EXPECT_NEAR(p->effect.condition.positiveSaturation, 1.0F, 1e-3F);
}

TEST(HidPid, FiniteEffectStopsByItselfAndKeepAlivesFollow) {
    Recorder r;
    r.pid.tick(0);
    r.pid.tick(500);
    EXPECT_TRUE(r.packets.empty());          // no keep-alives before a game used force feedback
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0400"), 0);
    r.pid.output(0x14, hex("00B80B0000000064000000"), 1000);          // sine 3000, period 100 ms
    r.pid.output(0x11, hex("0104DC05000000000000FFFF04FF1F0000FFFFFFFF"), 1000);   // 1500 ms
    r.pid.output(0x1A, hex("010101"), 1000);
    ASSERT_NE(r.lastPlaying(), nullptr);
    EXPECT_EQ(r.lastPlaying()->effect.type, FfbEffectType::Sine);
    EXPECT_NEAR(r.lastPlaying()->effect.periodic.magnitude, 0.3F, 1e-3F);
    EXPECT_NEAR(r.lastPlaying()->effect.periodic.periodMs, 100.0F, 1e-3F);
    r.packets.clear();
    r.pid.tick(2400);
    EXPECT_TRUE(r.packets.empty() || r.packets.back().playing || r.packets.back().kind == FfbPacket::Kind::KeepAlive);
    r.pid.tick(2600);                        // 1600 ms after start: over
    bool stopped = false;
    for (const auto& packet : r.packets) {
        stopped = stopped || (packet.kind == FfbPacket::Kind::SlotState && !packet.playing);
    }
    EXPECT_TRUE(stopped);
    r.packets.clear();
    r.pid.tick(2800);
    r.pid.tick(2950);
    bool keepAlive = false;
    for (const auto& packet : r.packets) keepAlive = keepAlive || packet.kind == FfbPacket::Kind::KeepAlive;
    EXPECT_TRUE(keepAlive);
}

// A crashed game sends nothing more: a pushing force must not be kept alive for ever.
TEST(HidPid, GameSilentWithAPushingForceStopsKeepAlives) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("00A00F"), 0);    // constant 0.4, infinite
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    ASSERT_NE(r.lastPlaying(), nullptr);
    for (std::int64_t t = 10; t < HidPid::kGameQuietMs; t += 10) r.pid.tick(t);
    EXPECT_FALSE(r.pid.gameQuiet());
    r.packets.clear();
    for (std::int64_t t = HidPid::kGameQuietMs; t < HidPid::kGameQuietMs + 2000; t += 10) r.pid.tick(t);
    EXPECT_TRUE(r.pid.gameQuiet());
    EXPECT_TRUE(r.packets.empty());          // nothing more: the service's watchdog stops the force
    r.pid.output(0x15, hex("00A00F"), HidPid::kGameQuietMs + 2000);   // the game again
    EXPECT_FALSE(r.pid.gameQuiet());
    r.pid.tick(HidPid::kGameQuietMs + 2300);
    EXPECT_FALSE(r.packets.empty());
}

TEST(HidPid, SpringAloneIsKeptAliveWhileTheGameIsSilent) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0800"), 0);
    r.pid.output(0x13, hex("000F000028232823041004100000"), 0);
    r.pid.output(0x11, hex("0108FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    for (std::int64_t t = 10; t < 3 * HidPid::kGameQuietMs; t += 10) r.pid.tick(t);
    EXPECT_FALSE(r.pid.gameQuiet());
    r.packets.clear();
    r.pid.tick(3 * HidPid::kGameQuietMs + 300);
    EXPECT_FALSE(r.packets.empty());
}

TEST(HidPid, PoolFullAndReset) {
    Recorder r;
    std::array<std::uint8_t, 8> feature{};
    for (std::size_t i = 0; i < HidPid::kMaxSlots; ++i) r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
    EXPECT_EQ(feature[1], HidPid::kMaxSlots);
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
    EXPECT_EQ(feature[2], 2);                // full
    r.pid.output(0x1C, hex("04"), 10);       // device reset
    EXPECT_EQ(r.packets.back().kind, FfbPacket::Kind::StopAll);
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 20);
    ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
    EXPECT_EQ(feature[1], 1);
    ASSERT_EQ(r.pid.getFeature(HidPid::kPool, feature), 5U);
    EXPECT_EQ(feature[3], HidPid::kMaxSlots);
    EXPECT_EQ(feature[4], 0x01);
    EXPECT_EQ(r.pid.getFeature(0x42, feature), 0U);
}

// Set Envelope comes with EBI 0 before the effect, like the other parameters; tick() sends the level as it changes.
TEST(HidPid, EnvelopeShapesAConstantForce) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x12, hex("0000000000F4010000F4010000"), 0);   // attack 0 over 500 ms, fade to 0 over 500 ms
    r.pid.output(0x15, hex("001027"), 0);                       // constant 10000
    r.pid.output(0x11, hex("0101D007000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // 2000 ms, 90 degrees
    r.pid.output(0x1A, hex("010101"), 0);
    ASSERT_NE(r.lastPlaying(), nullptr);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 0.0F, 1e-2F);
    r.pid.tick(250);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 0.5F, 1e-2F);
    r.pid.tick(1000);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 1.0F, 1e-2F);
    r.pid.tick(1750);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 0.5F, 1e-2F);
}

TEST(HidPid, RampMovesOverItsDuration) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x16, hex("0078EC8813"), 0);                   // ramp -5000 -> 5000
    r.pid.output(0x11, hex("0102D007000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // 2000 ms
    r.pid.output(0x1A, hex("010101"), 0);
    ASSERT_NE(r.lastPlaying(), nullptr);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, -0.5F, 1e-2F);
    r.pid.tick(500);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, -0.25F, 1e-2F);
    r.pid.tick(1000);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 0.0F, 1e-2F);
}

TEST(HidPid, AnEnvelopeBeforeASpringIsNotKeptForTheNextEffect) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x12, hex("0000000000F4010000F4010000"), 0);
    r.pid.output(0x13, hex("000F000070177017102710270000"), 0);
    r.pid.output(0x11, hex("0108B80B000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // a spring
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("001027"), 0);
    r.pid.output(0x11, hex("0201D007000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // then a constant, block 2
    r.pid.output(0x1A, hex("020101"), 0);
    ASSERT_NE(r.lastPlaying(), nullptr);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 1.0F, 1e-2F);   // full at once: no attack
}

TEST(HidPid, ABlockNumberNeverHandedOutTakesNoSlot) {
    Recorder r;
    r.pid.output(0x11, hex("2801DC05000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // Set Effect for block 40
    r.pid.output(0x1A, hex("280101"), 1);
    EXPECT_EQ(r.lastPlaying(), nullptr);
    EXPECT_TRUE(std::any_of(r.logs.begin(), r.logs.end(),
                            [](const std::string& line) { return line.find("never hands out") != std::string::npos; }));
    std::array<std::uint8_t, 8> feature{};
    for (std::size_t i = 0; i < HidPid::kMaxSlots; ++i) {   // all eight still free
        r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 10);
        ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
        EXPECT_EQ(feature[2], 1);   // success
    }
}

TEST(HidG920, InputReportPacksLikeTheBridge) {
    TargetReport report;
    report.steering = 65535;   // full right
    report.throttle = 0;       // throttle pressed
    report.brake = 65535;      // released
    report.clutch = 32768;
    report.buttons = 0x5FF | (1U << 12) | (1U << 18) | (1U << 20);   // Gear 1, Gear R, and one the G920 lacks
    report.hat = 2;
    const auto bytes = packG920Input(report);
    const std::array<std::uint8_t, kG920InputReportSize> expected = {
        0x01, 0xFF, 0x7F, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80, 0xFF, 0x15, 0x04, 0x02};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(packG920Input(g920Neutral())[12], 8);   // hat centred
    const auto descriptor = g920ReportDescriptor();
    ASSERT_EQ(descriptor.size(), 1150U);
    // The button block: Usage 1..19, 19 one-bit fields, then 5 constant padding bits before the hat.
    const std::array<std::uint8_t, 22> buttons = {0x05, 0x09, 0x19, 0x01, 0x29, 0x13, 0x15, 0x00, 0x25, 0x01, 0x95,
                                                  0x13, 0x75, 0x01, 0x81, 0x02, 0x95, 0x05, 0x75, 0x01, 0x81, 0x03};
    EXPECT_TRUE(std::search(descriptor.begin(), descriptor.end(), buttons.begin(), buttons.end()) != descriptor.end());
    EXPECT_EQ(descriptor[0], 0x05);          // Usage Page (Generic Desktop)
    EXPECT_EQ(descriptor[descriptor.size() - 1], 0xC0);
    // Out-of-range axis values are clamped, never overflow.
    TargetReport extreme;
    extreme.steering = std::numeric_limits<std::int32_t>::min();
    extreme.brake = std::numeric_limits<std::int32_t>::max();
    const auto clamped = packG920Input(extreme);
    EXPECT_EQ(clamped[1], 0x00);
    EXPECT_EQ(clamped[2], 0x80);   // -32768
    EXPECT_EQ(clamped[3], 0xFF);
    EXPECT_EQ(clamped[4], 0x7F);   // 32767
}

}  // namespace
}  // namespace wheelbri
