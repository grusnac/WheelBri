#include "wheelemu/HidG920.hpp"
#include "wheelemu/HidPid.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelemu {
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

// The reports Windows' pid.dll sent for tools/vbridge/FfbProbe (2026-09-30): parameters with EBI 0, then
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

TEST(HidG920, InputReportPacksLikeTheBridge) {
    TargetReport report;
    report.steering = 65535;   // full right
    report.throttle = 0;       // throttle pressed
    report.brake = 65535;      // released
    report.clutch = 32768;
    report.buttons = 0x5FF;
    report.hat = 2;
    const auto bytes = packG920Input(report);
    const std::array<std::uint8_t, kG920InputReportSize> expected = {
        0x01, 0xFF, 0x7F, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x80, 0xFF, 0x05, 0x02};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(packG920Input(g920Neutral())[11], 8);   // hat centred
    const auto descriptor = g920ReportDescriptor();
    ASSERT_EQ(descriptor.size(), 1144U);
    EXPECT_EQ(descriptor[0], 0x05);          // Usage Page (Generic Desktop)
    EXPECT_EQ(descriptor[descriptor.size() - 1], 0xC0);
}

}  // namespace
}  // namespace wheelemu
