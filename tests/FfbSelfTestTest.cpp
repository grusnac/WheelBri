// SPDX-License-Identifier: GPL-3.0-or-later
// --ffb-test (service/src/FfbSelfTest.cpp) on a fake sink: the four steps, their strength cap, an effect the wheel
// refuses, and Ctrl+C before, during and at the end of the sequence.

#include "FfbSelfTest.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace wheelbri::service {
namespace {

// Records what was played; refuses the effect types in `refused`.
class FakeSink : public IFfbSink {
public:
    std::set<FfbEffectType> refused;
    std::vector<FfbEffect> played;
    int stops = 0;
    int stopAlls = 0;

    bool play(std::size_t, const FfbEffect& effect) override {
        if (refused.count(effect.type) > 0) return false;
        played.push_back(effect);
        return true;
    }
    void stop(std::size_t) override { ++stops; }
    void stopAll() override { ++stopAlls; }

    [[nodiscard]] bool playedType(const FfbEffectType type) const {
        return std::any_of(played.begin(), played.end(), [type](const FfbEffect& e) { return e.type == type; });
    }
};

std::set<FfbEffectType> allTypes() {
    return {FfbEffectType::Spring, FfbEffectType::Constant, FfbEffectType::Damper};
}

// Every step plays for its 2 s and stops; no push goes past 20 % of the wheel, whatever the config allows.
TEST(FfbSelfTest, TheWholeSequencePlaysGently) {
    FakeSink sink;
    FfbSafetyConfig safety;
    safety.maxMagnitude = 1.0F;
    ::testing::internal::CaptureStdout();
    EXPECT_TRUE(runFfbSelfTest(sink, safety));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("spring: wheel should pull gently"), std::string::npos);
    EXPECT_NE(out.find("push LEFT"), std::string::npos);
    EXPECT_NE(out.find("push RIGHT"), std::string::npos);
    EXPECT_NE(out.find("damper"), std::string::npos);
    EXPECT_NE(out.find("self-test finished\n"), std::string::npos);
    EXPECT_EQ(sink.stops, 4);
    EXPECT_EQ(sink.stopAlls, 1);
    for (const FfbEffect& effect : sink.played) EXPECT_LE(std::fabs(effect.constantLevel), 0.2F + 1e-5F);
    EXPECT_TRUE(sink.playedType(FfbEffectType::Spring));
    EXPECT_TRUE(sink.playedType(FfbEffectType::Damper));
}

// A wheel that plays none of the effects: each step says so, the sequence still ends, and the result is false.
TEST(FfbSelfTest, EffectsTheWheelRefusesAreReported) {
    FakeSink sink;
    sink.refused = allTypes();
    ::testing::internal::CaptureStdout();
    EXPECT_FALSE(runFfbSelfTest(sink, FfbSafetyConfig{}));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("-> not supported by this device"), std::string::npos);
    EXPECT_NE(out.find("self-test finished with unsupported effects"), std::string::npos);
    EXPECT_EQ(sink.stops, 0);
    EXPECT_EQ(sink.stopAlls, 1);
}

// One effect refused and the rest played: the steps after it still run, and the result stays false.
TEST(FfbSelfTest, AFailedStepIsNotUndoneByLaterOnes) {
    FakeSink sink;
    sink.refused = {FfbEffectType::Spring};
    ::testing::internal::CaptureStdout();
    EXPECT_FALSE(runFfbSelfTest(sink, FfbSafetyConfig{}));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("self-test finished with unsupported effects"), std::string::npos);
    EXPECT_TRUE(sink.playedType(FfbEffectType::Constant));
    EXPECT_TRUE(sink.playedType(FfbEffectType::Damper));
    EXPECT_EQ(sink.stops, 3);
}

// Ctrl+C before anything: no step starts, every force is stopped, and the result is false.
TEST(FfbSelfTest, CancelledAtOnceStartsNothing) {
    FakeSink sink;
    ::testing::internal::CaptureStdout();
    EXPECT_FALSE(runFfbSelfTest(sink, FfbSafetyConfig{}, [] { return true; }));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_TRUE(sink.played.empty());
    EXPECT_NE(out.find("self-test stopped"), std::string::npos);
    EXPECT_EQ(out.find("spring:"), std::string::npos);
}

// Ctrl+C during a step: that step's force stops at once ("-> stopped") and no other step starts.
TEST(FfbSelfTest, CancelledDuringAStepStopsAtOnce) {
    FakeSink sink;
    int calls = 0;
    ::testing::internal::CaptureStdout();
    EXPECT_FALSE(runFfbSelfTest(sink, FfbSafetyConfig{}, [&calls] { return ++calls > 5; }));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("  -> stopped"), std::string::npos);
    EXPECT_EQ(sink.played.size(), 4U);   // the spring's first ticks only
    EXPECT_FALSE(sink.playedType(FfbEffectType::Constant));
    EXPECT_GE(sink.stopAlls, 2);
}

// Ctrl+C just as the last step ended: stopped, not "finished".
TEST(FfbSelfTest, CancelledAtTheEndIsStoppedToo) {
    FakeSink sink;
    sink.refused = allTypes();   // each step ends at its first tick: two checks per step
    int calls = 0;
    ::testing::internal::CaptureStdout();
    EXPECT_FALSE(runFfbSelfTest(sink, FfbSafetyConfig{}, [&calls] { return ++calls == 9; }));
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("self-test stopped"), std::string::npos);
    EXPECT_EQ(out.find("finished"), std::string::npos);
}

}  // namespace
}  // namespace wheelbri::service
