// SPDX-License-Identifier: GPL-3.0-or-later
// ChangeCounter: the count of announced changes and the waiting for the next one, with real threads.

#include "wheelbri/ChangeCounter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace wheelbri {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t msSince(const Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

TEST(ChangeCounter, StartsAtZeroAndCountsEachAnnouncement) {
    ChangeCounter counter;
    EXPECT_EQ(counter.count(), 0U);
    counter.announce();
    counter.announce();
    EXPECT_EQ(counter.count(), 2U);
}

TEST(ChangeCounter, AChangeAlreadyMadeEndsTheWaitAtOnce) {
    ChangeCounter counter;
    counter.announce();
    const auto start = Clock::now();
    EXPECT_TRUE(counter.waitForChange(0, 5'000));
    EXPECT_LT(msSince(start), 1'000U);
}

TEST(ChangeCounter, WithoutAChangeTheWaitEndsAtItsTimeout) {
    ChangeCounter counter;
    const auto start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 30));
    EXPECT_GE(msSince(start), 25U);
}

TEST(ChangeCounter, AZeroTimeoutOnlyLooks) {
    ChangeCounter counter;
    const auto start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 0));
    EXPECT_LT(msSince(start), 1'000U);
    counter.announce();
    EXPECT_TRUE(counter.waitForChange(0, 0));
}

TEST(ChangeCounter, AnAnnouncementFromAnotherThreadEndsTheWait) {
    ChangeCounter counter;
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        counter.announce();
    });
    const auto start = Clock::now();
    EXPECT_TRUE(counter.waitForChange(0, 10'000));
    EXPECT_LT(msSince(start), 5'000U);
    later.join();
}

TEST(ChangeCounter, WakeWaitersEndsAWaitWithoutAChange) {
    ChangeCounter counter;
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        counter.wakeWaiters();
    });
    const auto start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 10'000));
    EXPECT_LT(msSince(start), 5'000U);
    EXPECT_EQ(counter.count(), 0U);
    later.join();
}

TEST(ChangeCounter, AWakeBeforeTheWaitBeganDoesNotEndIt) {
    ChangeCounter counter;
    counter.wakeWaiters();
    const auto start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 30));
    EXPECT_GE(msSince(start), 25U);
}

TEST(ChangeCounter, WakesCountsEachWakeWaiters) {
    ChangeCounter counter;
    EXPECT_EQ(counter.wakes(), 0U);
    counter.wakeWaiters();
    counter.wakeWaiters();
    EXPECT_EQ(counter.wakes(), 2U);
    EXPECT_EQ(counter.count(), 0U);   // a wake is not a change
}

TEST(ChangeCounter, AWakeSinceWhatTheWaiterSawEndsTheWaitAtOnce) {
    // The waiter looked at its state (wakes() = 0), then was woken before it began to wait: not lost.
    ChangeCounter counter;
    const std::uint64_t wakesSeen = counter.wakes();
    counter.wakeWaiters();
    const auto start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 10'000, wakesSeen));
    EXPECT_LT(msSince(start), 1'000U);
}

TEST(ChangeCounter, WithTheCurrentWakesSeenOnlyALaterWakeEndsTheWait) {
    ChangeCounter counter;
    counter.wakeWaiters();
    const std::uint64_t wakesSeen = counter.wakes();
    auto start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 30, wakesSeen));   // the earlier wake does not count
    EXPECT_GE(msSince(start), 25U);
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        counter.wakeWaiters();
    });
    start = Clock::now();
    EXPECT_FALSE(counter.waitForChange(0, 10'000, wakesSeen));
    EXPECT_LT(msSince(start), 5'000U);
    later.join();
}

TEST(ChangeCounter, AChangeAndAWakeTogetherSayChanged) {
    ChangeCounter counter;
    const std::uint64_t wakesSeen = counter.wakes();
    counter.wakeWaiters();
    counter.announce();
    EXPECT_TRUE(counter.waitForChange(0, 10'000, wakesSeen));
}

TEST(ChangeCounter, EveryWaiterIsWoken) {
    ChangeCounter counter;
    std::atomic<int> changed{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 4; ++i) {
        waiters.emplace_back([&] {
            if (counter.waitForChange(0, 10'000)) ++changed;
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto start = Clock::now();
    counter.announce();
    for (std::thread& waiter : waiters) waiter.join();
    EXPECT_EQ(changed.load(), 4);
    EXPECT_LT(msSince(start), 5'000U);
}

TEST(ChangeCounter, AHugeTimeoutIsCutAndTheWaitStillEndsOnAChange) {
    // A timeout near the clock's limit must not overflow into "no wait at all" (or "for ever").
    // It did wait until the change: an overflow would have returned at once, false. (Not checked by the clock: on
    // Windows a 30 ms sleep can end after 16, a timer tick, and the test failed there on 2026-10-12.)
    ChangeCounter counter;
    std::atomic<bool> announced{false};
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        announced = true;
        counter.announce();
    });
    EXPECT_TRUE(counter.waitForChange(0, std::numeric_limits<std::uint64_t>::max()));
    EXPECT_TRUE(announced.load());
    later.join();
}

TEST(ChangeCounter, AnnouncementsFromManyThreadsAreAllCounted) {
    ChangeCounter counter;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1'000; ++i) counter.announce();
        });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(counter.count(), 8'000U);
}

}  // namespace
}  // namespace wheelbri
