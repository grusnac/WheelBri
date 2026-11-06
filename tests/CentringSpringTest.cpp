// SPDX-License-Identifier: GPL-3.0-or-later
// The target's centring spring (CentringSpring.hpp): only the G25 and G27 have one (Logitech's software did), [ffb]
// centring_spring = 0..100 in their mapping, and FfbController plays it in a slot of its own whatever the game does.

#include "wheelbri/CentringSpring.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace wheelbri;

namespace {

constexpr std::uint64_t kMs = 1'000'000;

class RecordingSink final : public IFfbSink {
public:
    bool play(const std::size_t slot, const FfbEffect& effect) override {
        plays.push_back({slot, effect});
        return accept;
    }
    void stop(const std::size_t slot) override { stops.push_back(slot); }
    void stopAll() override { ++stopAlls; }

    struct Play {
        std::size_t slot;
        FfbEffect effect;
    };
    [[nodiscard]] std::size_t springPlays() const {
        std::size_t n = 0;
        for (const Play& p : plays) n += p.slot == IFfbSink::kCentringSpringSlot ? 1 : 0;
        return n;
    }
    std::vector<Play> plays;
    std::vector<std::size_t> stops;
    int stopAlls = 0;
    bool accept = true;
};

FfbSafetyConfig limits(const float cap = 1.0F) {
    FfbSafetyConfig config;
    config.masterGain = 1.0F;
    config.maxMagnitude = cap;
    return config;
}

FfbPacket packet(const FfbPacket::Kind kind, const std::uint8_t slot = 0, const bool playing = false) {
    FfbPacket p;
    p.kind = kind;
    p.slot = slot;
    p.playing = playing;
    return p;
}

EmulatorConfigResult load(const std::string& text) { return loadEmulatorConfig(IniDocument::parse(text).document); }

}  // namespace

// A spring around the centre: no dead band, both sides the same, full saturation; 0..100 percent, NaN is off.
TEST(CentringSpring, TheEffectIsASpringAroundTheCentre) {
    const FfbEffect e = centringSpringEffect(25.0F);
    EXPECT_EQ(e.type, FfbEffectType::Spring);
    EXPECT_FLOAT_EQ(e.condition.center, 0.0F);
    EXPECT_FLOAT_EQ(e.condition.deadband, 0.0F);
    EXPECT_FLOAT_EQ(e.condition.positiveCoefficient, 0.25F);
    EXPECT_FLOAT_EQ(e.condition.negativeCoefficient, 0.25F);
    EXPECT_FLOAT_EQ(e.condition.positiveSaturation, 1.0F);
    EXPECT_FLOAT_EQ(e.condition.negativeSaturation, 1.0F);
    EXPECT_FLOAT_EQ(centringSpringEffect(150.0F).condition.positiveCoefficient, 1.0F);
    EXPECT_FLOAT_EQ(centringSpringEffect(-5.0F).condition.positiveCoefficient, 0.0F);
    EXPECT_FLOAT_EQ(centringSpringEffect(std::numeric_limits<float>::quiet_NaN()).condition.positiveCoefficient, 0.0F);
}

// Only the targets whose own software had one: the G25 and G27, not the G920 or G923.
TEST(CentringSpring, OnlyTheG25AndG27HaveOne) {
    for (const char* name : {"g25", "g27"}) {
        const auto profile = makeProfile(name);
        ASSERT_NE(profile, nullptr) << name;
        EXPECT_TRUE(profile->identity().centringSpring) << name;
    }
    for (const char* name : {"g920", "g923"}) {
        const auto profile = makeProfile(name);
        ASSERT_NE(profile, nullptr) << name;
        EXPECT_FALSE(profile->identity().centringSpring) << name;
    }
}

// [ffb] centring_spring: 0..100, off when left out; out of range or not a number is an error and stays off.
TEST(CentringSpring, TheConfigReadsZeroToAHundred) {
    EXPECT_FLOAT_EQ(load("").config.centringSpringPercent, 0.0F);
    const auto set = load("[ffb]\ncentring_spring = 25\n");
    EXPECT_TRUE(set.errors.empty());
    EXPECT_FLOAT_EQ(set.config.centringSpringPercent, 25.0F);
    for (const char* bad : {"101", "-1", "nan", "strong"}) {
        const auto result = load(std::string("[ffb]\ncentring_spring = ") + bad + "\n");
        EXPECT_FALSE(result.errors.empty()) << bad;
        EXPECT_FLOAT_EQ(result.config.centringSpringPercent, 0.0F) << bad;
    }
}

// The Mapping tab writes it to [ffb] and reads it back; a file that never had it gains no line while it is off.
TEST(CentringSpring, TheMappingWritesAndReadsIt) {
    const std::string base = "[emulator]\ntarget_profile = g25\n[ffb]\nmaster_gain = 1.0\n";
    MappingModel model = mappingModel(load(base).config);
    EXPECT_FLOAT_EQ(model.centringSpringPercent, 0.0F);
    const MappingWriteResult same = writeMapping(base, model);
    ASSERT_TRUE(same.error.empty()) << same.error;
    EXPECT_EQ(same.text.find("centring_spring"), std::string::npos);

    model.centringSpringPercent = 40.0F;
    const MappingWriteResult written = writeMapping(base, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    const auto again = load(written.text);
    EXPECT_TRUE(again.errors.empty());
    EXPECT_FLOAT_EQ(again.config.centringSpringPercent, 40.0F);

    model.centringSpringPercent = 120.0F;
    EXPECT_FALSE(writeMapping(base, model).error.empty());
}

// Played in its own slot, through the limiter (the cap holds its saturation), once: nothing changed, nothing re-sent.
TEST(CentringSpring, PlaysInItsOwnSlotThroughTheLimiter) {
    RecordingSink sink;
    FfbController controller(sink, limits(0.6F));
    controller.update(0);
    EXPECT_TRUE(sink.plays.empty());   // off until set
    controller.setCentringSpring(30.0F);
    controller.update(1 * kMs);
    ASSERT_EQ(sink.plays.size(), 1U);
    EXPECT_EQ(sink.plays[0].slot, IFfbSink::kCentringSpringSlot);
    const FfbEffect& sent = sink.plays[0].effect;
    EXPECT_EQ(sent.type, FfbEffectType::Spring);
    EXPECT_FLOAT_EQ(sent.condition.positiveCoefficient, 0.3F);
    EXPECT_FLOAT_EQ(sent.condition.positiveSaturation, 0.6F);   // the cap
    EXPECT_TRUE(controller.centringSpringPlaying());
    controller.update(100 * kMs);
    EXPECT_EQ(sink.plays.size(), 1U);

    controller.setCentringSpring(60.0F);   // stronger: sent again
    controller.update(200 * kMs);
    ASSERT_EQ(sink.springPlays(), 2U);
    EXPECT_FLOAT_EQ(sink.plays.back().effect.condition.positiveCoefficient, 0.6F);

    controller.setCentringSpring(0.0F);    // off: stopped
    controller.update(300 * kMs);
    ASSERT_EQ(sink.stops.size(), 1U);
    EXPECT_EQ(sink.stops[0], IFfbSink::kCentringSpringSlot);
    EXPECT_FALSE(controller.centringSpringPlaying());
}

// The game cannot take it away: its "stop all" and its silence (the watchdog) leave the spring playing.
TEST(CentringSpring, TheGameCannotStopIt) {
    RecordingSink sink;
    FfbController controller(sink, limits());
    controller.setCentringSpring(25.0F);
    controller.onPacket(packet(FfbPacket::Kind::StopAll), 0);
    controller.update(0);
    ASSERT_EQ(sink.springPlays(), 1U);
    controller.update(2000 * kMs);   // long past the watchdog
    EXPECT_TRUE(controller.watchdogTripped());
    EXPECT_TRUE(sink.stops.empty());
    EXPECT_TRUE(controller.centringSpringPlaying());
}

// WheelBri stopping everything (the loop ended, the wheel came back) stops it too; the next update plays it again.
TEST(CentringSpring, StopAllStopsItAndTheNextUpdatePlaysItAgain) {
    RecordingSink sink;
    FfbController controller(sink, limits());
    controller.setCentringSpring(25.0F);
    controller.update(0);
    controller.stopAll();
    EXPECT_EQ(sink.stopAlls, 1);
    EXPECT_FALSE(controller.centringSpringPlaying());
    controller.update(10 * kMs);
    EXPECT_EQ(sink.springPlays(), 2U);
}

// A wheel that refuses it is not asked every tick; setting it again (the next Start) tries again.
TEST(CentringSpring, ARefusedSpringIsNotRetriedEveryTick) {
    RecordingSink sink;
    sink.accept = false;
    FfbController controller(sink, limits());
    controller.setCentringSpring(25.0F);
    controller.update(0);
    controller.update(10 * kMs);
    controller.update(20 * kMs);
    EXPECT_EQ(sink.springPlays(), 1U);
    EXPECT_FALSE(controller.centringSpringPlaying());
    sink.accept = true;
    controller.setCentringSpring(25.0F);
    controller.update(30 * kMs);
    EXPECT_EQ(sink.springPlays(), 2U);
    EXPECT_TRUE(controller.centringSpringPlaying());
}
