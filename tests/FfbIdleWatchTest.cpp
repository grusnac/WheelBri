// FfbIdleWatch: the game loaded its force feedback but plays none while you drive (NFS 2015 at the start of a race).

#include "wheelbri/FfbIdleWatch.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace wheelbri {
namespace {

// Ticks of 1 ms from `from` until `until`, the pedal as `pedal(now)` says; the first change, and when it came.
struct Outcome {
    std::optional<bool> change;
    std::uint64_t at = 0;
};
template <typename Pedal>
Outcome drive(FfbIdleWatch& watch, const std::uint64_t from, const std::uint64_t until, Pedal pedal,
        const std::size_t loaded, const std::size_t wanted) {
    for (std::uint64_t now = from; now < until; ++now) {
        if (auto change = watch.update(now, pedal(now), loaded, wanted)) return {change, now};
    }
    return {std::nullopt, until};
}

TEST(FfbIdleWatch, WarnsAfterTenSecondsOfPedalWithEffectsLoadedAndNoneWanted) {
    FfbIdleWatch watch;
    const Outcome r = drive(watch, 1000, 60'000, [](const std::uint64_t now) { return (now / 1000) % 2 == 0; }, 3, 0);
    ASSERT_TRUE(r.change.has_value());
    EXPECT_TRUE(*r.change);
    EXPECT_GE(r.at, 1000U + 19'000U);   // 10 s of pedal took about 20 s
    EXPECT_LE(r.at, 1000U + 21'000U);
    EXPECT_TRUE(watch.warning());
    EXPECT_FALSE(watch.update(r.at + 1, true, 3, 0).has_value());   // said once
}

TEST(FfbIdleWatch, AnEffectStartedEndsTheWarning) {
    FfbIdleWatch watch;
    const Outcome r = drive(watch, 0, 30'000, [](std::uint64_t) { return true; }, 2, 0);
    ASSERT_EQ(r.change, true);
    EXPECT_EQ(watch.update(r.at + 1, true, 2, 1), false);   // the alt-tab: the game starts them
    EXPECT_FALSE(watch.warning());
    EXPECT_FALSE(watch.update(r.at + 2, true, 2, 1).has_value());
}

TEST(FfbIdleWatch, NothingLoadedAnyMoreEndsTheWarning) {
    FfbIdleWatch watch;
    const Outcome r = drive(watch, 0, 30'000, [](std::uint64_t) { return true; }, 2, 0);
    ASSERT_EQ(r.change, true);
    EXPECT_EQ(watch.update(r.at + 1, true, 0, 0), false);   // "stop all", back to the menus
    EXPECT_FALSE(watch.warning());
}

TEST(FfbIdleWatch, QuietWithoutPedalWithNothingLoadedOrWhileTheGamePlays) {
    FfbIdleWatch noPedal;
    EXPECT_FALSE(drive(noPedal, 0, 60'000, [](std::uint64_t) { return false; }, 3, 0).change.has_value());
    FfbIdleWatch nothingLoaded;   // another controller, or no force feedback at all: GameUseWatch's business
    EXPECT_FALSE(drive(nothingLoaded, 0, 60'000, [](std::uint64_t) { return true; }, 0, 0).change.has_value());
    FfbIdleWatch playing;
    EXPECT_FALSE(drive(playing, 0, 60'000, [](std::uint64_t) { return true; }, 3, 1).change.has_value());
    EXPECT_FALSE(playing.warning());
}

TEST(FfbIdleWatch, AWantedEffectStartsTheCountAgain) {
    FfbIdleWatch watch;
    EXPECT_FALSE(drive(watch, 0, 9'000, [](std::uint64_t) { return true; }, 3, 0).change.has_value());
    EXPECT_FALSE(watch.update(9'000, true, 3, 1).has_value());   // the game played it for a moment
    // 9 s were counted before; from zero again, so 9 more seconds are not enough.
    EXPECT_FALSE(drive(watch, 9'001, 18'001, [](std::uint64_t) { return true; }, 3, 0).change.has_value());
    const Outcome r = drive(watch, 18'001, 30'000, [](std::uint64_t) { return true; }, 3, 0);
    EXPECT_EQ(r.change, true);
}

TEST(FfbIdleWatch, NothingLoadedStartsTheCountAgain) {
    FfbIdleWatch watch;
    EXPECT_FALSE(drive(watch, 0, 9'000, [](std::uint64_t) { return true; }, 3, 0).change.has_value());
    EXPECT_FALSE(watch.update(9'000, true, 0, 0).has_value());
    EXPECT_FALSE(drive(watch, 9'001, 18'001, [](std::uint64_t) { return true; }, 3, 0).change.has_value());
}

TEST(FfbIdleWatch, AGapInTheLoopCountsAtMostATenthOfASecond) {
    FfbIdleWatch watch;
    EXPECT_FALSE(watch.update(0, true, 1, 0).has_value());
    EXPECT_FALSE(watch.update(60'000, true, 1, 0).has_value());   // the loop stalled a minute: 100 ms counted
    EXPECT_FALSE(watch.update(50'000, true, 1, 0).has_value());   // a clock that went back counts nothing
    const Outcome r = drive(watch, 50'001, 70'000, [](std::uint64_t) { return true; }, 1, 0);
    ASSERT_EQ(r.change, true);
    EXPECT_GE(r.at, 50'001U + 9'800U);
}

}  // namespace
}  // namespace wheelbri
