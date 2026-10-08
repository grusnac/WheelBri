#include "wheelbri/WaitFor.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace wheelbri {
namespace {

// A clock that only moves when the wait sleeps; a sleep can be cut short (`wakeAfterMs`, a device arriving).
struct FakeClock {
    std::uint64_t nowMs = 0;
    int sleeps = 0;
    std::uint64_t wakeAfterMs = 0;   // 0: every sleep runs its full time
    std::function<std::uint64_t(std::uint64_t)> sleep() {
        return [this](const std::uint64_t ms) {
            const std::uint64_t slept = wakeAfterMs != 0 ? std::min(ms, wakeAfterMs) : ms;
            nowMs += slept;
            ++sleeps;
            return slept;
        };
    }
};

TEST(WaitFor, AlreadyThereDoesNotWait) {
    FakeClock clock;
    std::uint64_t waited = 99;
    EXPECT_EQ(waitFor({}, [] { return true; }, [] { return false; }, clock.sleep(), &waited), WaitOutcome::Ready);
    EXPECT_EQ(clock.sleeps, 0);
    EXPECT_EQ(waited, 0U);
}

TEST(WaitFor, EndsAsSoonAsItAppears) {
    // The wheel comes back 2 s after Start (Windows restarting it).
    FakeClock clock;
    std::uint64_t waited = 0;
    const auto outcome = waitFor({15'000, 250}, [&] { return clock.nowMs >= 2'000; }, [] { return false; },
                                 clock.sleep(), &waited);
    EXPECT_EQ(outcome, WaitOutcome::Ready);
    EXPECT_EQ(waited, 2'000U);
    EXPECT_EQ(clock.sleeps, 8);
}

TEST(WaitFor, GivesUpAfterTheTimeout) {
    FakeClock clock;
    std::uint64_t waited = 0;
    int asked = 0;
    const auto outcome = waitFor({1'000, 300}, [&] { ++asked; return false; }, [] { return false; }, clock.sleep(),
                                 &waited);
    EXPECT_EQ(outcome, WaitOutcome::TimedOut);
    EXPECT_EQ(waited, 1'000U);   // 300 + 300 + 300 + 100: never longer than the timeout
    EXPECT_EQ(clock.nowMs, 1'000U);
    EXPECT_EQ(asked, 5);         // at once, after each sleep
}

TEST(WaitFor, StopCancelsTheWait) {
    FakeClock clock;
    const auto outcome = waitFor({15'000, 250}, [] { return false; }, [&] { return clock.nowMs >= 500; },
                                 clock.sleep());
    EXPECT_EQ(outcome, WaitOutcome::Cancelled);
    EXPECT_EQ(clock.nowMs, 500U);
}

TEST(WaitFor, ReadyWinsOverCancelledAtTheSameMoment) {
    // The wheel appeared and Stop was pressed in the same poll: the wheel is there, the session's own stop check
    // ends it right after.
    FakeClock clock;
    EXPECT_EQ(waitFor({}, [] { return true; }, [] { return true; }, clock.sleep()), WaitOutcome::Ready);
}

TEST(WaitFor, OnlyTheTimeReallyWaitedCountsTowardsTheTimeout) {
    // Device changes cut every sleep to 10 ms: the wait still lasts its full second, asking more often.
    FakeClock clock;
    clock.wakeAfterMs = 10;
    int asked = 0;
    std::uint64_t waited = 0;
    EXPECT_EQ(waitFor({1'000, 250}, [&] { ++asked; return false; }, [] { return false; }, clock.sleep(), &waited),
              WaitOutcome::TimedOut);
    EXPECT_EQ(waited, 1'000U);
    EXPECT_EQ(clock.nowMs, 1'000U);
    EXPECT_EQ(asked, 101);
}

TEST(WaitFor, ASleepThatEndsAtOnceStillMovesOn) {
    FakeClock clock;
    int sleeps = 0;
    std::uint64_t waited = 0;
    const auto outcome = waitFor({100, 50}, [] { return false; }, [] { return false; },
                                 [&](std::uint64_t) -> std::uint64_t { ++sleeps; return 0; }, &waited);
    EXPECT_EQ(outcome, WaitOutcome::TimedOut);
    EXPECT_EQ(sleeps, 100);   // 1 ms counted each time: it ends
    EXPECT_EQ(waited, 100U);
}

TEST(WaitFor, ZeroTimeoutAsksOnce) {
    FakeClock clock;
    int asked = 0;
    EXPECT_EQ(waitFor({0, 250}, [&] { ++asked; return false; }, [] { return false; }, clock.sleep()),
              WaitOutcome::TimedOut);
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(clock.sleeps, 0);
}

}  // namespace
}  // namespace wheelbri
