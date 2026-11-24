// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/MakerFfb.hpp"

#include "wheelbri/HidPid.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

using Report = std::vector<std::uint8_t>;

struct Ffb {
    std::vector<FfbPacket> packets;
    std::vector<std::string> lines;
    MakerFfb ffb{[this](const FfbPacket& p) { packets.push_back(p); },
                 [this](LogLevel, const std::string& line) { lines.push_back(line); }};

    void force(const std::uint16_t value, const std::int64_t nowMs) {
        const Report r{MakerFfb::kForceReport, static_cast<std::uint8_t>(value >> 8),
                       static_cast<std::uint8_t>(value & 0xFF), 0, 0};
        ffb.output(r, nowMs);
    }
    [[nodiscard]] const FfbPacket* last(const FfbPacket::Kind kind) const {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->kind == kind) return &*it;
        }
        return nullptr;
    }
    [[nodiscard]] int count(const FfbPacket::Kind kind) const {
        int n = 0;
        for (const auto& p : packets) n += p.kind == kind ? 1 : 0;
        return n;
    }
};

// GRID, 2026-11-24: above 0x7fff came with the wheel right of centre, so it pushes left.
TEST(MakerFfb, ForcesHighByteFirstAboveNoForcePushLeft) {
    EXPECT_FLOAT_EQ(MakerFfb::level(0x7FFF), 0.0F);
    EXPECT_FLOAT_EQ(MakerFfb::level(0xFFFF), -1.0F);
    EXPECT_FLOAT_EQ(MakerFfb::level(0x0000), 1.0F);
    Ffb f;
    f.force(0xBFFF, 0);
    const FfbPacket* p = f.last(FfbPacket::Kind::SlotState);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->playing);
    EXPECT_EQ(p->slot, 0);
    EXPECT_EQ(p->effect.type, FfbEffectType::Constant);
    EXPECT_NEAR(p->effect.constantLevel, -0.5F, 0.001F);
}

TEST(MakerFfb, SendsOnChangeRefreshesAndKeepsAlive) {
    Ffb f;
    f.ffb.tick(0);
    EXPECT_TRUE(f.packets.empty());   // nothing before a force
    f.force(0x7FFF, 0);
    f.force(0x7FFF, 10);
    EXPECT_EQ(f.count(FfbPacket::Kind::SlotState), 1);
    f.force(0x6000, 20);
    EXPECT_EQ(f.count(FfbPacket::Kind::SlotState), 2);
    for (std::int64_t t = 20; t <= 400; t += 10) {
        f.force(0x6000, t);
        f.ffb.tick(t);
    }
    EXPECT_GE(f.count(FfbPacket::Kind::SlotState), 3);   // a refresh
    f.packets.clear();
    for (std::int64_t t = 410; t <= 480; t += 10) f.ffb.tick(t);   // under the stream-quiet time
    for (std::int64_t t = 410; t <= 480; t += 10) f.ffb.tick(t);
    EXPECT_FALSE(f.packets.empty());
}

TEST(MakerFfb, AStoppedStreamWhilePushingStopsTheKeepAlives) {
    Ffb f;
    std::int64_t t = 0;
    for (; t <= HidPid::kStreamMinMs + 100; t += 20) {
        f.force(0x4000, t);
        f.ffb.tick(t);
    }
    const std::int64_t lastReport = t - 20;
    for (; t <= lastReport + HidPid::kStreamQuietMs + 50; t += 10) f.ffb.tick(t);
    f.packets.clear();
    for (std::int64_t u = t; u < t + 1000; u += 10) f.ffb.tick(u);
    EXPECT_TRUE(f.packets.empty());
    f.force(0x4000, t + 1000);
    EXPECT_FALSE(f.packets.empty());   // back
}

TEST(MakerFfb, OtherReportsAreLoggedOnceNotPlayed) {
    Ffb f;
    f.ffb.output(Report{0x41, 1, 2}, 0);
    f.ffb.output(Report{0x41, 1, 2}, 1);
    f.ffb.output(Report{0x40, 0x12}, 2);   // too short
    f.ffb.output(Report{}, 3);
    f.ffb.tick(500);
    EXPECT_TRUE(f.packets.empty());
    int notes = 0;
    for (const auto& l : f.lines) notes += l.find("report 0x41") != std::string::npos ? 1 : 0;
    EXPECT_EQ(notes, 1);
}

TEST(MakerFfb, StopAllStopsOnceAForceCame) {
    Ffb f;
    f.ffb.stopAll(0);
    EXPECT_TRUE(f.packets.empty());
    f.force(0x9000, 0);
    f.ffb.stopAll(10);
    EXPECT_NE(f.last(FfbPacket::Kind::StopAll), nullptr);
}

}  // namespace
}  // namespace wheelbri
