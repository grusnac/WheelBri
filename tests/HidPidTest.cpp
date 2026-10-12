#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidPid.hpp"

#include <gtest/gtest.h>

#include <optional>

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

}  // namespace
}  // namespace wheelbri
