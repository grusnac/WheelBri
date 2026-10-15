#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidPid.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <random>
#include <set>
#include <span>

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
    std::vector<LogLevel> levels;   // each line's, in the same order
    HidPid pid{[this](const FfbPacket& p) { packets.push_back(p); },
               [this](const LogLevel level, const std::string& s) {
                   logs.push_back(s);
                   levels.push_back(level);
               }};

    // The level the line starting with `start` was logged at (the first such line).
    [[nodiscard]] std::optional<LogLevel> levelOf(const std::string& start) const {
        for (std::size_t i = 0; i < logs.size(); ++i) {
            if (logs[i].rfind(start, 0) == 0) return levels[i];
        }
        return std::nullopt;
    }

    const FfbPacket* lastPlaying() const {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->kind == FfbPacket::Kind::SlotState && it->playing) return &*it;
        }
        return nullptr;
    }
};

// The trace is off unless asked for, and then writes exactly the lines asked for, plus its start and end.
TEST(HidPid, TraceIsOffUnlessEnabled) {
    const auto traced = [](const Recorder& r) {
        return std::count_if(r.logs.begin(), r.logs.end(),
                             [](const std::string& s) { return s.rfind("pid trace", 0) == 0; });
    };
    Recorder off;
    off.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    off.pid.output(0x15, hex("00A00F"), 0);
    EXPECT_EQ(traced(off), 0);

    Recorder on;
    on.pid.enableTrace(2);
    on.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    on.pid.output(0x15, hex("00A00F"), 0);
    on.pid.output(0x15, hex("00A00F"), 1);   // past the 2 lines: not traced
    EXPECT_EQ(traced(on), 4);                 // start, 2 lines, end
}

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
    r.pid.tick(100 + HidPid::kPendingMs);    // no Set Effect claimed it: the last constant gets it
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

// NFS Unbound streams its effects every 16 ms and, when it closes, simply stops: a held force under 1% (below the
// 10 s rule's "pushing" threshold) took the R3 to full lock (2026-10-12). A stream that stops is a game gone.
TEST(HidPid, AStreamThatStopsStopsKeepAlivesSoon) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("004A00"), 0);    // constant 0.0074
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    std::int64_t t = 0;
    for (; t < 3000; t += 16) {
        r.pid.output(0x15, hex("004A00"), t);
        r.pid.tick(t);
    }
    const std::int64_t last = t - 16;
    for (; t < last + HidPid::kStreamQuietMs; t += 10) r.pid.tick(t);
    EXPECT_FALSE(r.pid.gameQuiet());
    r.pid.tick(last + HidPid::kStreamQuietMs);
    EXPECT_TRUE(r.pid.gameQuiet());
    EXPECT_EQ(r.levelOf("pid: the game streamed force feedback"), LogLevel::Info);
    r.packets.clear();
    for (t = last + HidPid::kStreamQuietMs; t < last + 3000; t += 10) r.pid.tick(t);
    EXPECT_TRUE(r.packets.empty());          // nothing more: the service's watchdog stops the force
    r.pid.output(0x15, hex("004A00"), last + 3000);   // the game again
    EXPECT_FALSE(r.pid.gameQuiet());
    r.pid.tick(last + 3300);
    EXPECT_FALSE(r.packets.empty());
}

// A game that sets its effects and leaves them (a few reports, then silence) keeps the 10 s rule.
TEST(HidPid, AFewReportsAreNotAStream) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("00A00F"), 0);    // constant 0.4
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    for (std::int64_t t = 0; t <= 1000; t += 100) r.pid.output(0x15, hex("00A00F"), t);   // 1 s: too short
    for (std::int64_t t = 1010; t < 1000 + HidPid::kGameQuietMs; t += 10) r.pid.tick(t);
    EXPECT_FALSE(r.pid.gameQuiet());
    r.pid.tick(1000 + HidPid::kGameQuietMs);
    EXPECT_TRUE(r.pid.gameQuiet());
}

// A game at rest may stop sending while it holds only springs and dampers: those are not a game gone, they stay.
TEST(HidPid, AStreamOfSpringsAloneThatPausesKeepsGoing) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0800"), 0);
    r.pid.output(0x11, hex("0108FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    std::int64_t t = 0;
    for (; t < 3000; t += 16) {
        r.pid.output(0x13, hex("000F000028232823041004100000"), t);
        r.pid.tick(t);
    }
    for (; t < 6000; t += 10) r.pid.tick(t);
    EXPECT_FALSE(r.pid.gameQuiet());
    r.packets.clear();
    r.pid.tick(6300);
    EXPECT_FALSE(r.packets.empty());   // still kept alive
}

// pid.dll may send Block Free and Device Control as feature reports: they act as the output reports do.
TEST(HidPid, BlockFreeAndDeviceControlAsFeatureReports) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("00A00F"), 0);
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    ASSERT_NE(r.lastPlaying(), nullptr);
    r.pid.setFeature(0x1C, hex("03"), 10);   // device control: stop all effects
    EXPECT_EQ(r.pid.activeSlots(), 0U);
    r.pid.setFeature(0x1B, hex("01"), 20);   // block free: the block can be handed out again
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 30);
    std::array<std::uint8_t, 8> feature{};
    ASSERT_EQ(r.pid.getFeature(HidPid::kBlockLoad, feature), 5U);
    EXPECT_EQ(feature[1], 1);
    r.pid.setFeature(0x42, hex("00"), 40);   // a feature report this device does not have: nothing happens
    EXPECT_EQ(r.pid.activeSlots(), 0U);
}

TEST(HidPid, StateFeatureSaysActuatorsAndPause) {
    Recorder r;
    std::array<std::uint8_t, 8> out{};
    ASSERT_EQ(r.pid.getFeature(HidPid::kState, out), 3U);
    EXPECT_EQ(out[0], HidPid::kState);
    EXPECT_EQ(out[2], 0x12);   // actuators on, not paused
    r.pid.output(0x1C, hex("02"), 0);   // disable actuators
    r.pid.output(0x1C, hex("05"), 0);   // pause
    ASSERT_EQ(r.pid.getFeature(HidPid::kState, out), 3U);
    EXPECT_EQ(out[2], 0x11);
    EXPECT_EQ(r.pid.getFeature(HidPid::kState, std::span<std::uint8_t>(out.data(), 2)), 0U);   // too short
    ASSERT_EQ(r.pid.getFeature(HidPid::kCreateNewEffect, out), 1U);
    EXPECT_EQ(r.pid.getFeature(HidPid::kCreateNewEffect, std::span<std::uint8_t>()), 0U);
}

// Without a direction, a force acts along the enabled axes: X enabled = the wheel, else nothing on the wheel.
TEST(HidPid, ForceAlongTheEnabledAxesWithoutADirection) {
    const auto levelWithAxes = [](const char* axes) {
        Recorder r;
        r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
        r.pid.output(0x15, hex("00A00F"), 0);   // 0.4
        r.pid.output(0x11, hex(std::string("0101FFFF000000000000FFFF") + axes + "0000000000FFFFFFFF"), 0);
        r.pid.output(0x1A, hex("010101"), 0);
        r.pid.tick(10);
        const FfbPacket* const p = r.lastPlaying();
        return p != nullptr ? p->effect.constantLevel : -9.0F;
    };
    EXPECT_NEAR(levelWithAxes("01"), 0.4F, 1e-4F);   // X
    EXPECT_NEAR(levelWithAxes("02"), 0.0F, 1e-4F);   // Y only: nothing on the wheel
}

// Reports from a game are not trusted: any id, any length, any bytes. Nothing reads past them, the slots stay within
// the device's, and every packet sent is a well-formed one. Seeded: the same cases on every run.
TEST(HidPid, AnyReportsAreSafe) {
    std::mt19937 random(20261012U);
    Recorder r;
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 64> out{};
    for (std::int64_t t = 0; t < 20000; t += 1) {
        bytes.resize(random() % 24);
        for (std::uint8_t& b : bytes) b = static_cast<std::uint8_t>(random());
        if (!bytes.empty() && random() % 2 == 0) bytes[0] = static_cast<std::uint8_t>(random() % 10);   // real blocks
        const auto id = static_cast<std::uint8_t>(0x10 + random() % 16);
        switch (random() % 8) {
            case 0: r.pid.setFeature(id, bytes, t); break;
            case 1: (void)r.pid.getFeature(id, std::span<std::uint8_t>(out.data(), random() % out.size())); break;
            default: r.pid.output(id, bytes, t); break;
        }
        if (t % 7 == 0) r.pid.tick(t);
        ASSERT_LE(r.pid.activeSlots(), HidPid::kMaxSlots);
    }
    for (const FfbPacket& p : r.packets) {
        EXPECT_LT(p.slot, HidPid::kMaxSlots);
        EXPECT_TRUE(std::isfinite(p.effect.constantLevel));
        EXPECT_TRUE(std::isfinite(p.effect.periodic.magnitude));
        EXPECT_TRUE(std::isfinite(p.effect.periodic.periodMs));
        EXPECT_TRUE(std::isfinite(p.effect.condition.positiveCoefficient));
    }
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

// NFS on 2026-10-05: a spring and a damper (both conditions), each updated 30 times a second, as pid.dll sends it:
// Set Condition with block 0, then the Set Effect naming the block. Each kept its own values only some of the time
// (the other's landed on it first); now neither may ever play with the other's.
TEST(HidPid, SpringAndDamperUpdatesStayApart) {
    Recorder r;
    const std::string spring = "000F0000282328 23BC02BC020000";
    const std::string damper = "000F0000501450141027 10270000";
    const auto clean = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
        return s;
    };
    const std::string setSpring = "0108FFFF000000000000FFFF04FF1F0000FFFFFFFF";
    const std::string setDamper = "0209FFFF000000000000FFFF04FF1F0000FFFFFFFF";
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0800"), 0);
    r.pid.output(0x13, hex(clean(spring)), 0);
    r.pid.output(0x11, hex(setSpring), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0900"), 1);
    r.pid.output(0x13, hex(clean(damper)), 1);   // a new effect's: not the spring's, though it is the only condition
    r.pid.output(0x11, hex(setDamper), 1);
    r.pid.output(0x1A, hex("020101"), 1);
    for (std::int64_t t = 33; t < 1000; t += 33) {
        r.pid.output(0x13, hex(clean(spring)), t);
        r.pid.output(0x11, hex(setSpring), t);
        r.pid.output(0x13, hex(clean(damper)), t);
        r.pid.output(0x11, hex(setDamper), t);
        r.pid.tick(t + 10);
    }
    int springs = 0;
    int dampers = 0;
    for (const FfbPacket& p : r.packets) {
        if (p.kind != FfbPacket::Kind::SlotState || !p.playing) continue;
        if (p.effect.type == FfbEffectType::Spring) {
            ++springs;
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 0.9F, 1e-3F);
            EXPECT_NEAR(p.effect.condition.positiveSaturation, 0.07F, 1e-3F);
        } else if (p.effect.type == FfbEffectType::Damper) {
            ++dampers;
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 0.52F, 1e-3F);
            EXPECT_NEAR(p.effect.condition.positiveSaturation, 1.0F, 1e-3F);
        }
    }
    EXPECT_GT(springs, 0);
    EXPECT_GT(dampers, 0);
}

// A running effect changed without a Set Effect (as the probe saw for a constant force): with two of its kind the
// change goes to the last one set, once kPendingMs have passed without a Set Effect claiming it.
TEST(HidPid, AnUnclaimedUpdateStillArrives) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0800"), 0);
    r.pid.output(0x13, hex("000F000028232823102710270000"), 0);   // spring 0.9
    r.pid.output(0x11, hex("0108FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0900"), 1);
    r.pid.output(0x13, hex("000F000050145014102710270000"), 1);   // damper 0.52
    r.pid.output(0x11, hex("0209FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 1);
    r.pid.output(0x1A, hex("020101"), 1);
    r.pid.output(0x13, hex("000F0000A00FA00F102710270000"), 100);   // 0.4, no Set Effect after it
    r.pid.tick(105);
    const auto damperCoefficient = [&r] {
        for (auto it = r.packets.rbegin(); it != r.packets.rend(); ++it) {
            if (it->kind == FfbPacket::Kind::SlotState && it->effect.type == FfbEffectType::Damper) {
                return it->effect.condition.positiveCoefficient;
            }
        }
        return -1.0F;
    };
    EXPECT_NEAR(damperCoefficient(), 0.52F, 1e-3F);   // still waiting for a Set Effect
    r.pid.tick(100 + HidPid::kPendingMs);
    EXPECT_NEAR(damperCoefficient(), 0.4F, 1e-3F);    // the damper was set last
}

// The driver's trace of NFS on 2026-10-05 (reports exactly as Windows sent them): the new damper's parameters come
// before its first Set Effect, an Effect Operation can sit between them, and most reports come twice. Neither the
// spring (k 0.90, small saturation) nor the damper (saturation 1.0) may ever be sent with the other's values.
TEST(HidPid, NfsTraceKeepsSpringAndDamperApart) {
    Recorder r;
    const auto out = [&r](const char* id, const char* bytes, std::int64_t t) {
        r.pid.output(static_cast<std::uint8_t>(std::stoul(id, nullptr, 16)), hex(bytes), t);
    };
    // Set up (+622 .. +647): constant, spring, damper, sine.
    out("15", "000000", 622);
    out("12", "00000000000000000000000000", 623);
    out("11", "0101FFFF000000000000FFFF04FF1F0000FFFFFFFF", 624);
    out("1A", "010101", 626);
    out("13", "000F000028232823041004100000", 628);
    out("11", "0208FFFF000000000000FFFF04FF1F0000FFFFFFFF", 628);
    out("1A", "020101", 629);
    out("13", "000F000000000000102710270000", 630);   // the damper's, before its first Set Effect
    out("11", "0309FFFF000000000000FFFF04FF1F0000FFFFFFFF", 631);
    out("1A", "030101", 632);
    r.pid.tick(640);
    // Driving (+105777 ..): doubled reports, a start in between.
    out("1A", "020101", 105777);
    out("13", "000F000028232823041004100000", 105777);
    out("13", "000F000028232823041004100000", 105779);
    out("11", "0208FFFF000000000000FFFF04FF1F0000FFFFFFFF", 105779);
    out("11", "0208FFFF000000000000FFFF04FF1F0000FFFFFFFF", 105780);
    out("1A", "010101", 105781);
    out("13", "000F000000000000102710270000", 105784);
    out("1A", "030101", 105784);
    out("13", "000F000000000000102710270000", 105785);
    out("11", "0309FFFF000000000000FFFF04FF1F0000FFFFFFFF", 105786);
    r.pid.tick(105790);
    const char* rounds[][2] = {{"000F0000282328234C044C040000", "000F00005B125B12102710270000"},
                               {"000F000028232823830383030000", "000F000088138813102710270000"},
                               {"000F0000282328232C012C010000", "000F0000DF15DF15102710270000"},
                               {"000F000028232823C800C8000000", "000F0000A716A716102710270000"}};
    std::int64_t t = 105810;
    for (const auto& round : rounds) {
        for (int copy = 0; copy < 2; ++copy) out("13", round[0], t + copy);
        for (int copy = 0; copy < 2; ++copy) out("11", "0208FFFF000000000000FFFF04FF1F0000FFFFFFFF", t + 2 + copy);
        for (int copy = 0; copy < 2; ++copy) out("13", round[1], t + 4 + copy);
        for (int copy = 0; copy < 2; ++copy) out("11", "0309FFFF000000000000FFFF04FF1F0000FFFFFFFF", t + 6 + copy);
        r.pid.tick(t + 10);
        t += 33;
    }
    r.pid.tick(t + 100);
    int springs = 0;
    int dampers = 0;
    for (const FfbPacket& p : r.packets) {
        if (p.kind != FfbPacket::Kind::SlotState) continue;
        if (p.effect.type == FfbEffectType::Spring && p.effect.condition.positiveCoefficient != 0.0F) {
            ++springs;
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 0.9F, 1e-3F);
            EXPECT_LT(p.effect.condition.positiveSaturation, 0.5F);
        } else if (p.effect.type == FfbEffectType::Damper) {
            ++dampers;
            EXPECT_NEAR(p.effect.condition.positiveSaturation, 1.0F, 1e-3F);
        }
    }
    EXPECT_GT(springs, 4);
    EXPECT_GT(dampers, 4);
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

// --- what the game does with its effects, in the log (2026-10-08) ---

std::vector<std::string> events(const Recorder& r) {
    std::vector<std::string> lines;
    for (const std::string& line : r.logs) {
        if (line.rfind("pid: first ", 0) != 0 && line.rfind("pid: parameters come", 0) != 0) lines.push_back(line);
    }
    return lines;
}

bool has(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l == text; });
}

TEST(HidPid, EveryStepOfAnEffectIsLogged) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("00A00F"), 0);
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // NFS's constant: until stopped
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 5);   // the same again: not logged twice
    r.pid.output(0x1A, hex("0101FF"), 10);
    r.pid.output(0x1A, hex("010300"), 20);
    r.pid.output(0x1B, hex("01"), 30);
    const auto lines = events(r);
    EXPECT_TRUE(has(lines, "pid: new effect: block 1"));
    EXPECT_TRUE(has(lines, "pid: block 1 (constant, slot 0): until stopped, gain 100%"));
    EXPECT_TRUE(has(lines, "pid: start block 1 (constant, slot 0)"));
    EXPECT_TRUE(has(lines, "pid: stop block 1 (constant, slot 0)"));
    EXPECT_TRUE(has(lines, "pid: free block 1 (constant, slot 0)"));
    EXPECT_EQ(lines.size(), 5U);
}

TEST(HidPid, TimingAndLoopsAreSaid) {
    Recorder r;
    r.pid.output(0x11, hex("0203DC0500000000F401FFFF04FF1F0000FFFFFFFF"), 0);   // square, 1500 ms, 500 ms delay
    r.pid.output(0x1A, hex("020102"), 1);                                      // start, 2 times
    r.pid.output(0x11, hex("0203E80300000000F401FFFF04FF1F0000FFFFFFFF"), 2);   // now 1000 ms: logged again
    r.pid.output(0x1A, hex("020201"), 3);                                      // start solo
    const auto lines = events(r);
    EXPECT_TRUE(has(lines, "pid: block 2 (square, slot 0): 1500 ms, starts 500 ms after its start, gain 100%"));
    EXPECT_TRUE(has(lines, "pid: start block 2 (square, slot 0), 2 time(s)"));
    EXPECT_TRUE(has(lines, "pid: block 2 (square, slot 0): 1000 ms, starts 500 ms after its start, gain 100%"));
    EXPECT_TRUE(has(lines, "pid: start solo block 2 (square, slot 0), 1 time(s)"));
}

TEST(HidPid, AStartThatCannotPlayYetSaysWhy) {
    Recorder r;
    r.pid.output(0x1A, hex("0301FF"), 0);   // before its Set Effect
    r.pid.output(0x1C, hex("02"), 1);       // actuators off
    r.pid.output(0x1A, hex("0301FF"), 2);
    r.pid.output(0x1C, hex("05"), 3);       // paused
    r.pid.output(0x1A, hex("0301FF"), 4);
    r.pid.output(0x1A, hex("0901FF"), 5);   // a block this device never hands out
    r.pid.output(0x1A, hex("030700"), 6);   // an operation PID does not have
    const auto lines = events(r);
    EXPECT_TRUE(has(lines, "pid: start block 3 (no type yet, slot 0): its Set Effect has not come yet, it plays once "
                           "that does"));
    EXPECT_TRUE(has(lines, "pid: device control: disable actuators (no force until they are enabled again)"));
    EXPECT_TRUE(has(lines, "pid: start block 3 (no type yet, slot 0): its Set Effect has not come yet, it plays once "
                           "that does: the actuators are off (the game disabled them)"));
    EXPECT_TRUE(has(lines, "pid: device control: pause (no force until it continues)"));
    EXPECT_TRUE(has(lines, "pid: start block 3 (no type yet, slot 0): its Set Effect has not come yet, it plays once "
                           "that does: the actuators are off (the game disabled them): the device is paused (the "
                           "game paused it)"));
    EXPECT_TRUE(has(lines, "pid: effect operation 1 for block 9, a block this device does not have: ignored"));
    EXPECT_TRUE(has(lines, "pid: effect operation 7 for block 3 (no type yet, slot 0): unknown, ignored"));
}

TEST(HidPid, EveryDeviceCommandAndGainChangeIsSaid) {
    Recorder r;
    for (const char* command : {"01", "03", "06", "04", "09"}) r.pid.output(0x1C, hex(command), 0);
    r.pid.output(0x1D, hex("80"), 1);
    r.pid.output(0x1D, hex("80"), 2);   // unchanged: not again
    r.pid.output(0x1B, hex("05"), 3);   // free of a block not in use
    r.pid.setFeature(HidPid::kDeviceControl, hex("03"), 4);   // through the feature path too
    const auto lines = events(r);
    EXPECT_TRUE(has(lines, "pid: device control: enable actuators"));
    EXPECT_TRUE(has(lines, "pid: device control: stop all effects"));
    EXPECT_TRUE(has(lines, "pid: device control: continue"));
    EXPECT_TRUE(has(lines, "pid: device control: reset (every effect is gone)"));
    EXPECT_TRUE(has(lines, "pid: device control 9: unknown, ignored"));
    EXPECT_EQ(std::count(lines.begin(), lines.end(), "pid: device gain 50%"), 1);
    EXPECT_TRUE(has(lines, "pid: free block 5, which was not in use"));
    EXPECT_EQ(std::count(lines.begin(), lines.end(), "pid: device control: stop all effects"), 2);
}

TEST(HidPid, ARefusedNewEffectIsSaidEachTime) {
    Recorder r;
    for (std::size_t i = 0; i < HidPid::kMaxSlots + 2; ++i) r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    const auto lines = events(r);
    EXPECT_EQ(std::count(lines.begin(), lines.end(), "pid: new effect refused: all 8 effect blocks are in use"), 2);
}

TEST(HidPid, AFloodOfEventsIsCutAndCounted) {
    Recorder r;
    r.pid.output(0x11, hex("0103DC05000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    for (int i = 0; i < HidPid::kMaxEventLines + 50; ++i) r.pid.output(0x1A, hex("010101"), 1 + i);   // 150 starts
    auto lines = events(r);
    EXPECT_EQ(lines.size(), static_cast<std::size_t>(HidPid::kMaxEventLines));   // the Set Effect and 99 starts
    r.pid.output(0x1A, hex("010300"), HidPid::kEventWindowMs);   // the next window: the count first, then the line
    lines = events(r);
    EXPECT_EQ(lines[lines.size() - 2], "pid: 51 more effect events in 10 s were not logged");
    EXPECT_EQ(lines.back(), "pid: stop block 1 (square, slot 0)");
    r.pid.output(0x1A, hex("0101FF"), 2 * HidPid::kEventWindowMs + 1);   // a quiet window: no count line
    lines = events(r);
    EXPECT_EQ(lines.back(), "pid: start block 1 (square, slot 0)");
}

// The app's Level button: effect events at debug, the opt-in trace at trace, what goes wrong at warn, so the driver
// writes them only when its level allows (warn by default: warnings only).
TEST(HidPid, EachLineHasTheLevelItBelongsTo) {
    Recorder r;
    r.pid.enableTrace(2);
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("0101FF"), 10);
    r.pid.output(0x1A, hex("010700"), 11);   // an operation it does not know
    r.pid.output(0x1A, hex("090100"), 12);   // a block it does not have
    r.pid.output(0x1C, hex("09"), 13);       // a device control it does not know
    r.pid.output(0x1C, hex("05"), 14);       // pause
    r.pid.output(0x11, hex("2801DC05000000000000FFFF04FF1F0000FFFFFFFF"), 15);   // block 40: never handed out
    EXPECT_EQ(r.levelOf("pid trace"), LogLevel::Trace);
    EXPECT_EQ(r.levelOf("pid: first output"), LogLevel::Debug);
    EXPECT_EQ(r.levelOf("pid: new effect: block 1"), LogLevel::Debug);
    EXPECT_EQ(r.levelOf("pid: start block 1"), LogLevel::Debug);
    EXPECT_EQ(r.levelOf("pid: effect operation 7 for block 1"), LogLevel::Warn);
    EXPECT_EQ(r.levelOf("pid: effect operation 1 for block 9"), LogLevel::Warn);
    EXPECT_EQ(r.levelOf("pid: device control 9: unknown"), LogLevel::Warn);
    EXPECT_EQ(r.levelOf("pid: device control: pause"), LogLevel::Debug);
    EXPECT_EQ(r.levelOf("pid: a report for effect block 40"), LogLevel::Warn);
    Recorder full;
    for (int i = 0; i < 9; ++i) full.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), i);
    EXPECT_EQ(full.levelOf("pid: new effect refused"), LogLevel::Warn);
}

TEST(HidG920, InputReportPacksLikeARealG920) {
    TargetReport report;
    report.steering = 65535;   // full right
    report.throttle = 0;       // throttle pressed
    report.brake = 65535;      // released
    report.clutch = 32768;
    report.buttons = 0x5FF | (1U << 12) | (1U << 18) | (1U << 20);   // Gear 1, Gear R, and one the G920 lacks
    report.hat = 2;
    const auto bytes = packG920Input(report);
    // Wheel X, throttle Y (pressed: -32768), brake Rz (released: 32767), clutch Slider (half: 0).
    const std::array<std::uint8_t, kG920InputReportSize> expected = {
        0x01, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x00, 0xFF, 0x15, 0x04, 0x02};
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
    extreme.throttle = std::numeric_limits<std::int32_t>::max();
    const auto clamped = packG920Input(extreme);
    EXPECT_EQ(clamped[1], 0x00);
    EXPECT_EQ(clamped[2], 0x80);   // -32768
    EXPECT_EQ(clamped[3], 0xFF);
    EXPECT_EQ(clamped[4], 0x7F);   // 32767
}

// The G27's 23 buttons: the same descriptor and report, the button block declaring 23 buttons and 1 padding bit.
TEST(HidG920, TheG27DeclaresAndSends23Buttons) {
    const auto narrow = g920ReportDescriptor();
    const auto wide = g920ReportDescriptor(kG27Buttons);
    ASSERT_EQ(wide.size(), narrow.size());
    const std::array<std::uint8_t, 22> buttons = {0x05, 0x09, 0x19, 0x01, 0x29, 0x17, 0x15, 0x00, 0x25, 0x01, 0x95,
                                                  0x17, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x01, 0x81, 0x03};
    EXPECT_TRUE(std::search(wide.begin(), wide.end(), buttons.begin(), buttons.end()) != wide.end());
    std::size_t differences = 0;
    for (std::size_t i = 0; i < wide.size(); ++i) differences += wide[i] != narrow[i] ? 1 : 0;
    EXPECT_EQ(differences, 3U);   // Usage Maximum, Report Count, the padding's Report Count
    EXPECT_EQ(g920ReportDescriptor(kG25Buttons).data(), narrow.data());
    TargetReport report = g920Neutral();
    report.buttons = (1U << 19) | (1U << 22) | (1U << 23);   // R Wheel 2, L Wheel 3, and one the G27 lacks
    EXPECT_EQ(packG920Input(report, kG27Buttons)[11], 0x48);
    EXPECT_EQ(packG920Input(report)[11], 0x00);   // the G920's 19: none of them
}

// The four axis usages in report order (steering, brake, clutch, throttle): each axis starts Usage Page (Generic
// Desktop), Usage.
std::vector<std::uint8_t> axisUsages(const std::span<const std::uint8_t> descriptor) {
    std::vector<std::uint8_t> usages;
    for (std::size_t i = 8; i + 3 < descriptor.size() && usages.size() < 4; ++i) {
        if (descriptor[i] == 0x05 && descriptor[i + 1] == 0x01 && descriptor[i + 2] == 0x09) {
            usages.push_back(descriptor[i + 3]);
        }
    }
    return usages;
}

TEST(HidG920, ProtonDescriptorOnlyRenamesClutchAndThrottle) {
    const auto windows = g920ReportDescriptor();
    const auto proton = g920ProtonReportDescriptor();
    ASSERT_EQ(proton.size(), windows.size());
    std::size_t differences = 0;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        differences += windows[i] != proton[i] ? 1U : 0U;
    }
    EXPECT_EQ(differences, 2U);
    // Windows: X, Y, Rz, Slider (wheel, throttle, brake, clutch). Proton: X, Y, Z, Rz, the real G920's own HID layout,
    // which its G920 hack turns into X, Y, Rz, Slider.
    EXPECT_EQ(axisUsages(windows), (std::vector<std::uint8_t>{0x30, 0x31, 0x35, 0x36}));
    EXPECT_EQ(axisUsages(proton), (std::vector<std::uint8_t>{0x30, 0x31, 0x32, 0x35}));
}

// A game that streams parameters without Set Effects (NFS Unbound, 2026-10-11: a constant force every 16 ms, EBI 0):
// each value lands within about kPendingMs, not only when the game pauses.
TEST(HidPid, AStreamOfUnclaimedUpdatesKeepsArriving) {
    Recorder r;
    r.pid.setFeature(HidPid::kCreateNewEffect, hex("0100"), 0);
    r.pid.output(0x15, hex("000000"), 0);
    r.pid.output(0x11, hex("0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);
    r.pid.output(0x1A, hex("010101"), 0);
    std::int64_t tick = 0;
    for (int i = 1; i <= 40; ++i) {
        const std::int64_t t = 16 * i;
        for (; tick + 10 <= t; tick += 10) r.pid.tick(tick + 10);
        const std::int16_t level = static_cast<std::int16_t>(i * 100);   // 0.01 more each time
        const std::vector<std::uint8_t> bytes = {0x00, static_cast<std::uint8_t>(level & 0xFF),
                                                 static_cast<std::uint8_t>((level >> 8) & 0xFF)};
        r.pid.output(0x15, bytes, t);
    }
    for (; tick < 16 * 40 + 40; tick += 10) r.pid.tick(tick + 10);
    std::set<int> levels;
    for (const FfbPacket& p : r.packets) {
        if (p.kind == FfbPacket::Kind::SlotState && p.playing && p.effect.type == FfbEffectType::Constant) {
            levels.insert(static_cast<int>(std::lround(p.effect.constantLevel * 100.0F)));
        }
    }
    EXPECT_GE(levels.size(), 15U);   // about every other value (one each ~30 ms), not one at the end
    ASSERT_NE(r.lastPlaying(), nullptr);
    EXPECT_NEAR(r.lastPlaying()->effect.constantLevel, 0.40F, 1e-3F);   // and the last one
}

// Two conditions updated one after the other with EBI 0 (a damper, then a spring) go to the conditions in block
// order when nothing tells them apart; Windows' copy of the second (the same bytes again) is not a third update, and
// the third, idle spring keeps its own values.
TEST(HidPid, ARunOfUnclaimedUpdatesGoesToTheStartedBlocksInOrder) {
    Recorder r;
    const auto create = [&r](const char* setEffect, const char* start, std::int64_t t) {
        r.pid.setFeature(HidPid::kCreateNewEffect, hex("0900"), t);
        r.pid.output(0x13, hex("000F000000000000000000000000"), t);
        r.pid.output(0x11, hex(setEffect), t);
        if (start != nullptr) r.pid.output(0x1A, hex(start), t);
    };
    create("0109FFFF000000000000FFFF04FF1F0000FFFFFFFF", "010101", 0);   // block 1 damper
    create("0208FFFF000000000000FFFF04FF1F0000FFFFFFFF", "020101", 1);   // block 2 spring
    create("0308FFFF000000000000FFFF04FF1F0000FFFFFFFF", nullptr, 2);    // block 3 spring, never started
    for (std::int64_t t = 16; t < 400; t += 16) {
        r.pid.output(0x15, hex("000000"), t);                               // another kind ends any run
        r.pid.output(0x13, hex("000F0000C409C409C409C4090000"), t + 1);   // damper 0.25
        r.pid.output(0x13, hex("000F00004B004B004B004B000000"), t + 2);   // spring 0.0075
        r.pid.output(0x13, hex("000F00004B004B004B004B000000"), t + 2);   // Windows' copy
        r.pid.tick(t + 5);
        r.pid.tick(t + 15);
    }
    int dampers = 0;
    int springs = 0;
    for (const FfbPacket& p : r.packets) {
        if (p.kind != FfbPacket::Kind::SlotState || !p.playing) continue;
        if (p.effect.type == FfbEffectType::Damper && p.effect.condition.positiveCoefficient > 0.0F) {
            ++dampers;
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 0.25F, 1e-4F);
        } else if (p.effect.type == FfbEffectType::Spring && p.effect.condition.positiveCoefficient > 0.0F) {
            ++springs;
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 0.0075F, 1e-4F);
            EXPECT_EQ(p.slot, 1);   // block 2's slot: never the unstarted block 3
        }
    }
    EXPECT_GT(dampers, 0);
    EXPECT_GT(springs, 0);
}

// The driver's trace of NFS Unbound on 2026-10-11 (an excerpt, reports exactly as Windows sent them, ms from the
// first): six effects set up, then only EBI-0 parameters every ~16 ms: constant, damper (0.25), spring (falling with
// speed), triangle. Each goes to its own effect, and the idle spring (block 5) keeps its own values.
TEST(HidPid, UnboundTraceStreamsToTheRightEffects) {
    struct Report {
        std::int64_t t;
        std::uint8_t id;
        const char* bytes;
    };
    const std::vector<Report> trace = {
        {0, 0x1C, "04"},
        {2, 0x1C, "04"},
        {2, 0x1D, "FF"},
        {4, 0x1D, "FF"},
        {4, 0x1D, "FF"},
        {6, 0x15, "008813"},
        {7, 0x11, "0101FFFF000000000000FFFF04FF1F0000FFFFFFFF"},
        {9, 0x14, "0000000000000000000000"},
        {9, 0x11, "0204FFFF000000000000FFFF04FF1F0000FFFFFFFF"},
        {11, 0x13, "000F000000000000000000000000"},
        {11, 0x11, "0309FFFF000000000000FFFF04FF1F0000FFFFFFFF"},
        {13, 0x13, "000F000000000000000000000000"},
        {13, 0x11, "0408FFFF000000000000FFFF04FF1F0000FFFFFFFF"},
        {14, 0x13, "000F000010271027102710271027"},
        {15, 0x11, "0508FFFF000000000000FFFF04FF1F0000FFFFFFFF"},
        {16, 0x1A, "050101"},
        {18, 0x14, "0010270000000032000000"},
        {18, 0x12, "001027000000000000F4010000"},
        {19, 0x11, "0604F401000000000000FFFF04FF1F0000FFFFFFFF"},
        {65294, 0x1A, "010101"},
        {65306, 0x1A, "040101"},
        {65314, 0x1A, "030101"},
        {65320, 0x1A, "020300"},
        {65338, 0x11, "0205FFFF000000000000FFFF04FF1F0000FFFFFFFF"},
        {65348, 0x1A, "020101"},
        {75100, 0x14, "0018000000000021000000"},
        {75116, 0x15, "006401"},
        {75117, 0x13, "000F0000C409C409C409C4090000"},
        {75118, 0x13, "000F00004B004B004B004B000000"},
        {75119, 0x14, "0018000000000021000000"},
        {75132, 0x15, "005D01"},
        {75134, 0x13, "000F0000C409C409C409C4090000"},
        {75135, 0x13, "000F00004A004A004A004A000000"},
        {75136, 0x14, "0018000000000021000000"},
        {75148, 0x15, "005C01"},
        {75149, 0x13, "000F0000C409C409C409C4090000"},
        {75150, 0x13, "000F000049004900490049000000"},
        {75151, 0x14, "0018000000000021000000"},
        {75164, 0x15, "005701"},
        {75165, 0x13, "000F0000C409C409C409C4090000"},
        {75166, 0x13, "000F000048004800480048000000"},
        {75167, 0x14, "0018000000000021000000"},
        {75180, 0x15, "005201"},
        {75181, 0x13, "000F0000C409C409C409C4090000"},
        {75182, 0x13, "000F000046004600460046000000"},
        {75182, 0x14, "0018000000000021000000"},
        {75196, 0x15, "004E01"},
        {75197, 0x13, "000F0000C409C409C409C4090000"},
        {75198, 0x13, "000F000045004500450045000000"},
        {75199, 0x14, "0018000000000021000000"},
        {75217, 0x15, "004B01"},
        {75219, 0x13, "000F0000C409C409C409C4090000"},
        {75220, 0x13, "000F000044004400440044000000"},
        {75221, 0x14, "0019000000000021000000"},
        {75232, 0x15, "004801"},
        {75233, 0x13, "000F0000C409C409C409C4090000"},
        {75233, 0x13, "000F000042004200420042000000"},
        {75234, 0x14, "0019000000000021000000"},
        {75249, 0x15, "004001"},
        {75250, 0x13, "000F0000C409C409C409C4090000"},
        {75251, 0x13, "000F000041004100410041000000"},
        {75252, 0x14, "0019000000000021000000"},
        {75264, 0x15, "003F01"},
        {75265, 0x13, "000F0000C409C409C409C4090000"},
        {75266, 0x13, "000F00003F003F003F003F000000"},
        {75267, 0x14, "0019000000000021000000"},
        {75281, 0x15, "003D01"},
        {75282, 0x13, "000F0000C409C409C409C4090000"},
        {75282, 0x13, "000F00003E003E003E003E000000"},
        {75284, 0x14, "0019000000000021000000"},
        {75296, 0x15, "003C01"},
        {75297, 0x13, "000F0000C409C409C409C4090000"},
        {75298, 0x13, "000F00003D003D003D003D000000"},
        {75298, 0x14, "0019000000000021000000"},
        {75317, 0x15, "003B01"},
        {75319, 0x13, "000F0000C409C409C409C4090000"},
        {75319, 0x13, "000F00003B003B003B003B000000"},
        {75320, 0x13, "000F00003B003B003B003B000000"},
        {75321, 0x14, "001A000000000021000000"},
        {75332, 0x15, "003A01"},
        {75333, 0x13, "000F0000C409C409C409C4090000"},
        {75334, 0x13, "000F00003A003A003A003A000000"},
        {75335, 0x14, "001A000000000021000000"},
        {75348, 0x15, "003901"},
        {75349, 0x13, "000F0000C409C409C409C4090000"},
        {75350, 0x13, "000F000038003800380038000000"},
        {75351, 0x14, "001A000000000021000000"},
        {75364, 0x15, "003801"},
        {75365, 0x13, "000F0000C409C409C409C4090000"},
        {75366, 0x13, "000F000037003700370037000000"},
        {75367, 0x14, "001A000000000021000000"},
        {75381, 0x15, "003D01"},
        {75382, 0x13, "000F0000C409C409C409C4090000"},
        {75382, 0x13, "000F000035003500350035000000"},
        {75383, 0x14, "001A000000000021000000"},
        {75396, 0x15, "004101"},
        {75398, 0x13, "000F0000C409C409C409C4090000"},
        {75398, 0x13, "000F000034003400340034000000"},
        {75399, 0x14, "001A000000000021000000"},
    };
    Recorder r;
    std::int64_t tick = 0;
    for (const Report& report : trace) {
        for (; tick + 10 <= report.t; tick += 10) r.pid.tick(tick + 10);
        r.pid.output(report.id, hex(report.bytes), report.t);
    }
    for (int i = 0; i < 5; ++i) r.pid.tick(tick += 10);
    std::set<int> constants;
    int dampers = 0;
    int springs = 0;
    for (const FfbPacket& p : r.packets) {
        if (p.kind != FfbPacket::Kind::SlotState) continue;
        if (p.effect.type == FfbEffectType::Constant && p.playing) {
            constants.insert(static_cast<int>(std::lround(p.effect.constantLevel * 10000.0F)));
        } else if (p.effect.type == FfbEffectType::Damper && p.playing && p.effect.condition.positiveCoefficient > 0) {
            ++dampers;
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 0.25F, 1e-4F);   // never the spring's
        } else if (p.effect.type == FfbEffectType::Spring && p.slot == 3 && p.effect.condition.positiveCoefficient > 0) {
            ++springs;
            EXPECT_LT(p.effect.condition.positiveCoefficient, 0.01F);            // never the damper's
        } else if (p.effect.type == FfbEffectType::Spring && p.slot == 4) {
            EXPECT_NEAR(p.effect.condition.positiveCoefficient, 1.0F, 1e-4F);    // the idle spring, untouched
        }
    }
    EXPECT_GE(constants.size(), 10U);   // the steering force follows the stream (it used to wait for a pause)
    EXPECT_GT(dampers, 0);
    EXPECT_GT(springs, 0);
}

// Unbound, 2026-10-11 (+70047 in the trace): in the middle of a race the game starts sending its damper and spring
// the other way round. Each value goes on to the block it continues (the damper slowly falling from 0.25, the spring
// slowly rising from 0), never swapping them.
TEST(HidPid, UnclaimedUpdatesFollowTheirEffectWhenTheOrderChanges) {
    Recorder r;
    r.pid.output(0x13, hex("000F000000000000000000000000"), 0);
    r.pid.output(0x11, hex("0109FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 0);   // block 1 damper
    r.pid.output(0x1A, hex("010101"), 0);
    r.pid.output(0x13, hex("000F000000000000000000000000"), 1);
    r.pid.output(0x11, hex("0208FFFF000000000000FFFF04FF1F0000FFFFFFFF"), 1);   // block 2 spring
    r.pid.output(0x1A, hex("020101"), 1);
    const auto condition = [](const int value) {   // k and saturation both `value` / 10000
        char text[32];
        std::snprintf(text, sizeof(text), "000F0000%02X%02X%02X%02X%02X%02X%02X%02X0000", value & 0xFF, value >> 8,
                      value & 0xFF, value >> 8, value & 0xFF, value >> 8, value & 0xFF, value >> 8);
        return hex(text);
    };
    std::int64_t t = 16;
    for (int i = 0; i < 10; ++i, t += 16) {   // damper first
        r.pid.output(0x15, hex("000000"), t);
        r.pid.output(0x13, condition(2500), t + 1);
        r.pid.output(0x13, condition(0), t + 2);
        r.pid.tick(t + 5);
        r.pid.tick(t + 15);
    }
    for (int i = 0; i < 20; ++i, t += 16) {   // spring first: rising, while the damper falls
        r.pid.output(0x15, hex("000000"), t);
        r.pid.output(0x13, condition(20 * i), t + 1);
        r.pid.output(0x13, condition(2500 - 20 * i), t + 2);
        r.pid.tick(t + 5);
        r.pid.tick(t + 15);
    }
    for (const FfbPacket& p : r.packets) {
        if (p.kind != FfbPacket::Kind::SlotState || !p.playing) continue;
        const float k = p.effect.condition.positiveCoefficient;
        if (p.effect.type == FfbEffectType::Damper && k > 0.0F) {
            EXPECT_GE(k, 0.2F);
        } else if (p.effect.type == FfbEffectType::Spring) {
            EXPECT_LE(k, 0.05F);
        }
    }
}

}  // namespace
}  // namespace wheelbri
