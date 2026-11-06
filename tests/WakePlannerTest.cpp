// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/WakePlanner.hpp"

#include <gtest/gtest.h>

#include <array>

namespace wheelbri {
namespace {

constexpr std::uint64_t kMs = 1'000'000;

TEST(WakePlanner, IdleWakesEvery100ms) {
    WakeState state;
    state.lastPassNs = 1000 * kMs;
    EXPECT_EQ(nextWakeNs(state, WakeSettings{}), 1100 * kMs);
}

TEST(WakePlanner, AnOpenWindowWakesAbout30TimesASecond) {
    WakeState state;
    state.lastPassNs = 0;
    state.windowOpen = true;
    EXPECT_EQ(nextWakeNs(state, WakeSettings{}), WakeSettings{}.windowNs);
}

TEST(WakePlanner, PlayingForcesWinOverEverythingElse) {
    WakeState state;
    state.lastPassNs = 10 * kMs;
    state.windowOpen = true;
    state.forcesPlaying = true;
    EXPECT_EQ(nextWakeNs(state, WakeSettings{}), 14 * kMs);
}

TEST(WakePlanner, NeverCloserThanTheMinimumGap) {
    WakeSettings settings;
    settings.ffbNs = 100'000;   // asked for less than the flood guard allows
    WakeState state;
    state.lastPassNs = 0;
    state.forcesPlaying = true;
    EXPECT_EQ(nextWakeNs(state, settings), settings.minGapNs);
    EXPECT_EQ(earliestPassNs(state, settings), settings.minGapNs);
}

TEST(WakeStats, CountsByReasonAndDescribes) {
    WakeStats stats;
    stats.record(WakeReason::Wheel, 20'000);
    stats.record(WakeReason::Wheel, 40'000);
    stats.record(WakeReason::Timer, 30'000);
    stats.record(WakeReason::GameFfb, 10'000);
    EXPECT_EQ(stats.wakes(), 4U);
    EXPECT_EQ(stats.count(WakeReason::Wheel), 2U);
    EXPECT_EQ(stats.describe(2'000'000'000ULL),
              "4 wake-ups in 2 s (2 a second): wheel 2, game force feedback 1, timer 1; work avg 25.0 us, max 40.0 us");
    stats.reset();
    EXPECT_EQ(stats.wakes(), 0U);
}

// --- nextWakeNs: every combination of what must run on time ---

TEST(WakePlanner, OnlyForcesPlayingWakesEvery4ms) {
    WakeState state;
    state.lastPassNs = 5 * kMs;
    state.forcesPlaying = true;
    EXPECT_EQ(nextWakeNs(state, WakeSettings{}), 9 * kMs);
}

TEST(WakePlanner, TheShortestIntervalWinsWhicheverItIs) {
    WakeSettings settings;
    settings.idleNs = 10 * kMs;
    settings.windowNs = 50 * kMs;   // longer than idle: idle still wins
    settings.ffbNs = 60 * kMs;
    WakeState state;
    state.windowOpen = true;
    state.forcesPlaying = true;
    EXPECT_EQ(nextWakeNs(state, settings), 10 * kMs);
}

TEST(WakePlanner, AllIntervalsBelowTheGapGiveTheGap) {
    WakeSettings settings;
    settings.idleNs = 0;
    settings.windowNs = 0;
    settings.ffbNs = 0;
    WakeState state;
    state.lastPassNs = 7 * kMs;
    state.windowOpen = true;
    EXPECT_EQ(nextWakeNs(state, settings), 7 * kMs + settings.minGapNs);
}

TEST(WakePlanner, AnIntervalEqualToTheGapIsKept) {
    WakeSettings settings;
    settings.ffbNs = settings.minGapNs;
    WakeState state;
    state.forcesPlaying = true;
    EXPECT_EQ(nextWakeNs(state, settings), settings.minGapNs);
}

TEST(WakePlanner, RecentInputWakesEvery4msButForcesAndTheGapStillApply) {
    WakeState state;
    state.lastPassNs = 20 * kMs;
    state.inputRecent = true;
    EXPECT_EQ(nextWakeNs(state, WakeSettings{}), 24 * kMs);
    WakeSettings settings;
    settings.recentInputEveryNs = 0;   // below the gap
    EXPECT_EQ(nextWakeNs(state, settings), 20 * kMs + settings.minGapNs);
    settings.recentInputEveryNs = 50 * kMs;   // longer than forces playing: forces win
    state.forcesPlaying = true;
    EXPECT_EQ(nextWakeNs(state, settings), 24 * kMs);
}

// --- floodGuardWaitNs: what is left of the minimum gap ---

TEST(WakePlanner, FloodGuardWaitsForTheRestOfTheGap) {
    WakeState state;
    state.lastPassNs = 100 * kMs;
    const WakeSettings settings;
    EXPECT_EQ(floodGuardWaitNs(state, settings, 100 * kMs), settings.minGapNs);           // woke at once
    EXPECT_EQ(floodGuardWaitNs(state, settings, 100 * kMs + 200'000), 300'000U);          // part of the gap gone
    EXPECT_EQ(floodGuardWaitNs(state, settings, 100 * kMs + settings.minGapNs), 0U);      // exactly at the gap
    EXPECT_EQ(floodGuardWaitNs(state, settings, 105 * kMs), 0U);                          // long after
}

TEST(WakePlanner, FloodGuardWithAClockBehindThePassWaitsTheWholeGapAtMost) {
    WakeState state;
    state.lastPassNs = 100 * kMs;
    const WakeSettings settings;
    EXPECT_EQ(floodGuardWaitNs(state, settings, 99 * kMs), kMs + settings.minGapNs);
}

// --- wakeReasonFor ---

TEST(WakePlanner, WakeReasonForMapsIndexesAndTreatsTheRestAsTimer) {
    const std::array<WakeReason, 3> reasons{WakeReason::Wheel, WakeReason::GameFfb, WakeReason::Wheel};
    EXPECT_EQ(wakeReasonFor(0, reasons), WakeReason::Wheel);
    EXPECT_EQ(wakeReasonFor(1, reasons), WakeReason::GameFfb);
    EXPECT_EQ(wakeReasonFor(2, reasons), WakeReason::Wheel);    // an accessory counts as the wheel
    EXPECT_EQ(wakeReasonFor(3, reasons), WakeReason::Timer);    // past the end
    EXPECT_EQ(wakeReasonFor(-1, reasons), WakeReason::Timer);   // timed out
    EXPECT_EQ(wakeReasonFor(-2, reasons), WakeReason::Timer);   // the wait failed
    EXPECT_EQ(wakeReasonFor(0, std::span<const WakeReason>{}), WakeReason::Timer);
}

// --- Periodic ---

TEST(Periodic, FirstCallIsDueThenOncePerInterval) {
    Periodic every(10 * kMs);
    EXPECT_EQ(every.intervalNs(), 10 * kMs);
    EXPECT_TRUE(every.due(0));             // the first call, even at time 0
    EXPECT_FALSE(every.due(0));
    EXPECT_FALSE(every.due(9 * kMs));
    EXPECT_TRUE(every.due(10 * kMs));      // exactly one interval later
    EXPECT_FALSE(every.due(15 * kMs));
    EXPECT_TRUE(every.due(50 * kMs));      // long after: once, not a burst for the missed ones
    EXPECT_FALSE(every.due(55 * kMs));
    EXPECT_TRUE(every.due(60 * kMs));      // counted from when it last fired
}

TEST(Periodic, ByTheClockNotByTheNumberOfPasses) {
    Periodic every(33 * kMs);
    int fired = 0;
    for (std::uint64_t t = 0; t < 1000 * kMs; t += 4 * kMs) fired += every.due(t) ? 1 : 0;   // 250 passes
    int firedSparse = 0;
    Periodic sparse(33 * kMs);
    for (std::uint64_t t = 0; t < 1000 * kMs; t += 100 * kMs) firedSparse += sparse.due(t) ? 1 : 0;   // 10 passes
    EXPECT_EQ(fired, 28);         // every 36 ms (the first pass at or after each interval)
    EXPECT_EQ(firedSparse, 10);   // each pass, since each is past the interval
}

TEST(Periodic, AClockGoneBackStartsTheIntervalAgain) {
    Periodic every(10 * kMs);
    EXPECT_TRUE(every.due(100 * kMs));
    EXPECT_FALSE(every.due(50 * kMs));    // not "due" from an unsigned wrap-around
    EXPECT_FALSE(every.due(59 * kMs));
    EXPECT_TRUE(every.due(60 * kMs));
}

TEST(Periodic, ZeroIntervalIsAlwaysDue) {
    Periodic every(0);
    EXPECT_TRUE(every.due(5));
    EXPECT_TRUE(every.due(5));
}

// --- LossTimer ---

TEST(LossTimer, KeepsTheDeviceForHalfASecondOfFailures) {
    LossTimer loss;
    EXPECT_EQ(loss.failures(), 0U);
    EXPECT_TRUE(loss.failed(1000));       // first failure starts the clock
    EXPECT_TRUE(loss.failed(1200));
    EXPECT_TRUE(loss.failed(1499));       // still under 500 ms
    EXPECT_FALSE(loss.failed(1500));      // 500 ms: gone
    EXPECT_FALSE(loss.failed(9000));
    EXPECT_EQ(loss.failures(), 5U);
}

TEST(LossTimer, ByTheClockEvenWithFewReads) {
    LossTimer loss;
    EXPECT_TRUE(loss.failed(0));
    EXPECT_FALSE(loss.failed(600));       // the second failed read, already too late
}

TEST(LossTimer, SuccessReturnsTheFailuresAndStartsAgain) {
    LossTimer loss;
    EXPECT_EQ(loss.succeeded(), 0U);      // nothing failed
    EXPECT_TRUE(loss.failed(0));
    EXPECT_TRUE(loss.failed(400));
    EXPECT_EQ(loss.succeeded(), 2U);
    EXPECT_EQ(loss.failures(), 0U);
    EXPECT_TRUE(loss.failed(450));        // a new run of failures: its own 500 ms from here
    EXPECT_TRUE(loss.failed(900));
    EXPECT_FALSE(loss.failed(950));
}

TEST(LossTimer, CustomLimitAndClockGoneBack) {
    LossTimer loss(100);
    EXPECT_TRUE(loss.failed(1000));
    EXPECT_TRUE(loss.failed(500));        // clock went back: start again rather than wrap around
    EXPECT_TRUE(loss.failed(599));
    EXPECT_FALSE(loss.failed(600));
}

// --- NotificationCheck ---

TEST(NotificationCheck, OnlyTimerWakesWithNewInputCount) {
    NotificationCheck check(3, 10 * kMs);
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(check.record(WakeReason::Wheel, true, kMs));          // the wheel's own notification
        EXPECT_FALSE(check.record(WakeReason::GameFfb, true, kMs));
        EXPECT_FALSE(check.record(WakeReason::Timer, false, kMs));         // timer, but nothing changed
    }
    EXPECT_EQ(check.missed(), 0U);
    EXPECT_FALSE(check.failed());
}

TEST(NotificationCheck, ReportsOnceWhenTheThresholdIsReached) {
    NotificationCheck check(3, 10 * kMs);
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 1 * kMs));
    EXPECT_FALSE(check.record(WakeReason::Wheel, true, 2 * kMs));   // in between: does not reset the count
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 3 * kMs));
    EXPECT_EQ(check.missed(), 2U);
    EXPECT_TRUE(check.record(WakeReason::Timer, true, 4 * kMs));
    EXPECT_TRUE(check.failed());
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 5 * kMs));   // only once
}

TEST(NotificationCheck, MissesSpreadOverMoreThanTheWindowAreForgiven) {
    NotificationCheck check(3, 10 * kMs);
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 0));
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 9 * kMs));
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 10 * kMs));   // the window ran out: this is the first again
    EXPECT_EQ(check.missed(), 1U);
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 15 * kMs));
    EXPECT_TRUE(check.record(WakeReason::Timer, true, 19 * kMs));    // 3 within 10 ms
}

TEST(NotificationCheck, TheWindowStartsAtTheFirstMiss) {
    NotificationCheck check(2, 10 * kMs);
    EXPECT_FALSE(check.record(WakeReason::Wheel, true, 0));          // no miss yet: no window yet
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 50 * kMs));
    EXPECT_TRUE(check.record(WakeReason::Timer, true, 55 * kMs));
}

TEST(NotificationCheck, AClockGoneBackStartsANewWindow) {
    NotificationCheck check(2, 10 * kMs);
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 100 * kMs));
    EXPECT_FALSE(check.record(WakeReason::Timer, true, 50 * kMs));
    EXPECT_EQ(check.missed(), 1U);
}

TEST(NotificationCheck, DefaultsAre20MissesIn10Seconds) {
    NotificationCheck check;
    for (std::uint32_t i = 1; i < NotificationCheck::kThreshold; ++i) {
        EXPECT_FALSE(check.record(WakeReason::Timer, true, i * 100 * kMs));
    }
    EXPECT_TRUE(check.record(WakeReason::Timer, true, NotificationCheck::kThreshold * 100 * kMs));
}

TEST(WakeStats, DescribesNoWakesAndAZeroWindowWithoutDividingByZero) {
    const WakeStats stats;
    EXPECT_EQ(stats.describe(0),
              "0 wake-ups in 0 s (0 a second): wheel 0, game force feedback 0, timer 0; work avg 0.0 us, max 0.0 us");
}

}  // namespace
}  // namespace wheelbri
