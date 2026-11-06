// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/EventLoopControl.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace wheelbri {
namespace {

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::array<WakeReason, 2> kReasons{WakeReason::Wheel, WakeReason::GameFfb};
constexpr int kWheel = 0;
constexpr int kGameFfb = 1;

EventLoopControl::Pass passAt(const std::uint64_t startNs, const bool inputChanged = false) {
    EventLoopControl::Pass pass;
    pass.startNs = startNs;
    pass.inputChanged = inputChanged;
    return pass;
}

TEST(EventLoopControl, StartsOnEventsWithoutPolling) {
    const EventLoopControl control;
    EXPECT_FALSE(control.polling());
    EXPECT_EQ(control.pollReason(), PollReason::None);
    EXPECT_TRUE(control.canWait());
}

TEST(EventLoopControl, ASetupThatCannotWaitForEverythingPollsAndKeepsTheFirstReason) {
    EventLoopControl control;
    control.pollBecause(PollReason::None);   // nothing
    EXPECT_FALSE(control.polling());
    control.pollBecause(PollReason::NoNotification);
    control.pollBecause(PollReason::SocketNotWaitable);
    EXPECT_TRUE(control.polling());
    EXPECT_EQ(control.pollReason(), PollReason::NoNotification);
    EXPECT_TRUE(control.canWait());   // it still waits for everything else
}

TEST(EventLoopControl, DescribesEveryReason) {
    for (const PollReason reason : {PollReason::None, PollReason::NoNotification, PollReason::SocketNotWaitable,
                                    PollReason::WaitFailed, PollReason::MissedNotifications}) {
        EXPECT_NE(std::string(describe(reason)), "unknown");
    }
    EXPECT_EQ(std::string(describe(static_cast<PollReason>(99))), "unknown");
}

TEST(EventLoopControl, DeadlineFollowsWhatMustRunAndRecentInput) {
    EventLoopControl control;
    control.afterPass(passAt(0));
    EXPECT_EQ(control.deadlineNs(), 100 * kMs);   // idle

    control.afterPass(passAt(10 * kMs, true));
    EXPECT_EQ(control.deadlineNs(), 14 * kMs);    // new input: every 4 ms
    control.afterPass(passAt(109 * kMs));
    EXPECT_EQ(control.deadlineNs(), 113 * kMs);   // still within 100 ms of it
    control.afterPass(passAt(110 * kMs));
    EXPECT_EQ(control.deadlineNs(), 210 * kMs);   // 100 ms passed: idle again

    EventLoopControl::Pass window = passAt(300 * kMs);
    window.windowOpen = true;
    control.afterPass(window);
    EXPECT_EQ(control.deadlineNs(), 300 * kMs + 16'666'666);
    EventLoopControl::Pass forces = passAt(400 * kMs);
    forces.forcesPlaying = true;
    control.afterPass(forces);
    EXPECT_EQ(control.deadlineNs(), 404 * kMs);
}

TEST(EventLoopControl, WhilePollingTheDeadlineIsAMillisecondAtMost) {
    EventLoopControl control;
    control.pollBecause(PollReason::NoNotification);
    control.afterPass(passAt(0));
    EXPECT_EQ(control.deadlineNs(), 1 * kMs);   // instead of 100 ms idle
    control.afterPass(passAt(10 * kMs, true));
    EXPECT_EQ(control.deadlineNs(), 11 * kMs);  // instead of 4 ms after new input

    EventLoopControl::Settings settings;
    settings.pollNs = 100'000;   // below the flood guard: the flood guard wins
    EventLoopControl fast(settings);
    fast.pollBecause(PollReason::NoNotification);
    fast.afterPass(passAt(0));
    EXPECT_EQ(fast.deadlineNs(), 500'000U);
}

TEST(EventLoopControl, AfterWaitGivesTheReasonAndTheFloodGuard) {
    EventLoopControl control;
    control.afterPass(passAt(100 * kMs));
    EventLoopControl::Woke woke = control.afterWait(kGameFfb, kReasons, 100 * kMs + 200'000);
    EXPECT_EQ(woke.reason, WakeReason::GameFfb);
    EXPECT_EQ(woke.sleepNs, 300'000U);
    EXPECT_FALSE(woke.startedPolling);
    EXPECT_EQ(control.wokeBy(), WakeReason::GameFfb);

    woke = control.afterWait(kWaitTimedOut, kReasons, 200 * kMs);
    EXPECT_EQ(woke.reason, WakeReason::Timer);
    EXPECT_EQ(woke.sleepNs, 0U);
    woke = control.afterWait(7, kReasons, 200 * kMs);   // an index nothing was added for
    EXPECT_EQ(woke.reason, WakeReason::Timer);
}

TEST(EventLoopControl, AFailedWaitStopsTheWaitingOnce) {
    EventLoopControl control;
    control.afterPass(passAt(0));
    EventLoopControl::Woke woke = control.afterWait(kWaitFailed, kReasons, 1 * kMs);
    EXPECT_TRUE(woke.startedPolling);
    EXPECT_EQ(control.pollReason(), PollReason::WaitFailed);
    EXPECT_FALSE(control.canWait());
    EXPECT_EQ(woke.reason, WakeReason::Timer);
    woke = control.afterWait(kWaitFailed, kReasons, 2 * kMs);
    EXPECT_FALSE(woke.startedPolling);   // already: nothing new to log
}

TEST(EventLoopControl, AFailedWaitWhilePollingForAnotherReasonIsSaidToo) {
    EventLoopControl control;
    control.pollBecause(PollReason::NoNotification);
    control.afterPass(passAt(0));
    EXPECT_TRUE(control.afterWait(kWaitFailed, kReasons, 1 * kMs).startedPolling);   // the loop must stop waiting
    EXPECT_EQ(control.pollReason(), PollReason::WaitFailed);
    EXPECT_FALSE(control.canWait());
}

TEST(EventLoopControl, TheFirstPassSaysNothingAboutNotifications) {
    EventLoopControl control;
    control.afterPass(passAt(0, true));   // the timer "woke" it: it ran before any wait
    (void)control.afterWait(kWaitTimedOut, kReasons, 100 * kMs);
    EXPECT_EQ(control.missedNotifications(), 0U);
}

TEST(EventLoopControl, ANotificationThatComesSoonAfterTheTimerPassWasLate) {
    EventLoopControl control;
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs);
    control.afterPass(passAt(4 * kMs, true));                        // the timer found new input
    (void)control.afterWait(kGameFfb, kReasons, 4 * kMs + 300'000);   // something else first: still undecided
    control.afterPass(passAt(4 * kMs + 300'000));
    (void)control.afterWait(kWheel, kReasons, 4 * kMs + 800'000);     // its notification, within 1 ms
    EXPECT_EQ(control.lateNotifications(), 1U);
    EXPECT_EQ(control.missedNotifications(), 0U);
}

TEST(EventLoopControl, ANotificationThatNeverComesWasMissed) {
    EventLoopControl control;
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs);
    control.afterPass(passAt(4 * kMs, true));
    (void)control.afterWait(kWheel, kReasons, 6 * kMs);   // a notification, but 2 ms later: another change's
    EXPECT_EQ(control.missedNotifications(), 1U);
    EXPECT_EQ(control.lateNotifications(), 0U);
}

TEST(EventLoopControl, TimerPassesWithoutNewInputCountForNothing) {
    EventLoopControl control;
    for (std::uint64_t t = 0; t < 10'000 * kMs; t += 100 * kMs) {
        control.afterPass(passAt(t));
        (void)control.afterWait(kWaitTimedOut, kReasons, t + 100 * kMs);
    }
    EXPECT_EQ(control.missedNotifications(), 0U);
    EXPECT_FALSE(control.polling());
}

TEST(EventLoopControl, TwentyMissedWithinTheWindowStartPolling) {
    EventLoopControl::Settings settings;
    settings.missThreshold = 3;
    EventLoopControl control(settings);
    control.afterPass(passAt(0));
    std::uint64_t t = 4 * kMs;
    bool polling = false;
    int waitsUntilPolling = 0;
    (void)control.afterWait(kWaitTimedOut, kReasons, t);
    while (!polling && waitsUntilPolling < 10) {
        control.afterPass(passAt(t, true));
        t += 4 * kMs;
        polling = control.afterWait(kWaitTimedOut, kReasons, t).startedPolling;
        ++waitsUntilPolling;
    }
    EXPECT_TRUE(polling);
    EXPECT_EQ(waitsUntilPolling, 3);
    EXPECT_EQ(control.pollReason(), PollReason::MissedNotifications);
    EXPECT_TRUE(control.canWait());   // it still waits for the notifications that do come

    // Once polling, nothing more is decided here.
    control.afterPass(passAt(t, true));
    EXPECT_FALSE(control.afterWait(kWaitTimedOut, kReasons, t + 4 * kMs).startedPolling);
}

TEST(EventLoopControl, WhilePollingMissesAreNotCounted) {
    EventLoopControl control;
    control.pollBecause(PollReason::NoNotification);
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 1 * kMs);
    control.afterPass(passAt(1 * kMs, true));
    (void)control.afterWait(kWaitTimedOut, kReasons, 5 * kMs);
    EXPECT_EQ(control.missedNotifications(), 0U);
}

// A pass timed before the last new input (a clock that stepped back): the input is not "recent", the loop goes idle.
TEST(EventLoopControl, APassBeforeTheLastInputIsNotRecentInput) {
    EventLoopControl control;
    control.afterPass(passAt(10 * kMs, true));
    EXPECT_EQ(control.deadlineNs(), 14 * kMs);
    control.afterPass(passAt(5 * kMs));
    EXPECT_EQ(control.deadlineNs(), 105 * kMs);
}

// The timer finds new input twice before the notification: the first pending miss stands and one late notification
// settles it.
TEST(EventLoopControl, ASecondTimerPassWhileANotificationIsPendingKeepsTheFirst) {
    EventLoopControl control;
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs);
    control.afterPass(passAt(4 * kMs, true));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs + 400'000);   // the timer again, still in time
    control.afterPass(passAt(4 * kMs + 400'000, true));
    (void)control.afterWait(kWheel, kReasons, 4 * kMs + 900'000);          // 0.9 ms after the first: late
    EXPECT_EQ(control.lateNotifications(), 1U);
    EXPECT_EQ(control.missedNotifications(), 0U);
}

// A wake timed before the pass that left a notification pending (clock trouble) cannot be its late notification.
TEST(EventLoopControl, AWakeTimedBeforeThePendingPassIsNotLate) {
    EventLoopControl control;
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs);
    control.afterPass(passAt(4 * kMs, true));
    (void)control.afterWait(kWheel, kReasons, 3 * kMs);
    EXPECT_EQ(control.lateNotifications(), 0U);
    EXPECT_EQ(control.missedNotifications(), 1U);
}

// Polling started for another reason while a miss was pending: the miss is counted but does not start polling
// again (the reason stays the first one).
TEST(EventLoopControl, AMissDecidedWhileAlreadyPollingKeepsTheReason) {
    EventLoopControl::Settings settings;
    settings.missThreshold = 1;
    EventLoopControl control(settings);
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs);
    control.afterPass(passAt(4 * kMs, true));
    control.pollBecause(PollReason::SocketNotWaitable);
    const EventLoopControl::Woke woke = control.afterWait(kWaitTimedOut, kReasons, 10 * kMs);
    EXPECT_FALSE(woke.startedPolling);
    EXPECT_EQ(control.missedNotifications(), 1U);
    EXPECT_EQ(control.pollReason(), PollReason::SocketNotWaitable);
}

}  // namespace
}  // namespace wheelbri
