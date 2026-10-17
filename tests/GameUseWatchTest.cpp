#include "wheelbri/GameUseWatch.hpp"

#include <gtest/gtest.h>

using namespace wheelbri;

TEST(GameUseWatch, WarnsAfterTwentySecondsOfPedalWithoutForceFeedback) {
    GameUseWatch watch;
    std::uint64_t now = 1000;
    std::optional<bool> change;
    for (int i = 0; i < 60'000 && !change; ++i) {   // 1 ms ticks, the pedal pressed only every other second
        now += 1;
        change = watch.update(now, (now / 1000) % 2 == 0, 0);
    }
    ASSERT_TRUE(change.has_value());
    EXPECT_TRUE(*change);
    EXPECT_GE(now, 1000U + 39'000U);   // 20 s of pedal took about 40 s
    EXPECT_TRUE(watch.warning());
    EXPECT_FALSE(watch.update(now + 1, true, 0).has_value());   // said once
    EXPECT_EQ(watch.update(now + 2, true, 3), false);           // the game sends force feedback: over
    EXPECT_FALSE(watch.warning());
}

TEST(GameUseWatch, QuietWhileTheGameSendsOrNoPedalIsPressed) {
    GameUseWatch watch;
    for (std::uint64_t now = 0; now < 60'000; now += 1) {
        EXPECT_FALSE(watch.update(now, false, 0).has_value());
    }
    GameUseWatch sending;
    for (std::uint64_t now = 0; now < 60'000; now += 1) {
        EXPECT_FALSE(sending.update(now, true, 5).has_value());
    }
    GameUseWatch paused;   // a long pause between ticks (the PC asleep) does not count as pedal time
    EXPECT_FALSE(paused.update(0, true, 0).has_value());
    EXPECT_FALSE(paused.update(600'000, true, 0).has_value());
}

// Two passes of the loop within the same millisecond, or a clock that went back, add no pedal time.
TEST(GameUseWatch, NoTimePassingAddsNoPedalTime) {
    GameUseWatch watch;
    EXPECT_FALSE(watch.update(5'000, true, 0).has_value());
    for (int i = 0; i < 100'000; ++i) {
        EXPECT_FALSE(watch.update(5'000, true, 0).has_value());
    }
    EXPECT_FALSE(watch.update(1'000, true, 0).has_value());
    EXPECT_FALSE(watch.warning());
}
