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

TEST(EventLoopControl, StartsOnEventsOrNotWanted) {
    EXPECT_TRUE(EventLoopControl(true).events());
    EXPECT_EQ(EventLoopControl(true).fallback(), LoopFallback::None);
    EXPECT_FALSE(EventLoopControl(false).events());
    EXPECT_EQ(EventLoopControl(false).fallback(), LoopFallback::NotWanted);
}

TEST(EventLoopControl, SetupFailureKeepsTheFirstReason) {
    EventLoopControl control(true);
    control.setupFailed(LoopFallback::None);   // nothing
    EXPECT_TRUE(control.events());
    control.setupFailed(LoopFallback::NoNotification);
    control.setupFailed(LoopFallback::SocketNotWaitable);
    EXPECT_EQ(control.fallback(), LoopFallback::NoNotification);

    EventLoopControl fixed(false);
    fixed.setupFailed(LoopFallback::NoNotification);
    EXPECT_EQ(fixed.fallback(), LoopFallback::NotWanted);
}

TEST(EventLoopControl, DescribesEveryFallback) {
    for (const LoopFallback fallback :
         {LoopFallback::None, LoopFallback::NotWanted, LoopFallback::NoNotification, LoopFallback::SocketNotWaitable,
          LoopFallback::WaitFailed, LoopFallback::MissedNotifications}) {
        EXPECT_NE(std::string(describe(fallback)), "unknown");
    }
    EXPECT_EQ(std::string(describe(static_cast<LoopFallback>(99))), "unknown");
}

TEST(EventLoopControl, DeadlineFollowsWhatMustRunAndRecentInput) {
    EventLoopControl control(true);
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
    EXPECT_EQ(control.deadlineNs(), 300 * kMs + 33'333'333);
    EventLoopControl::Pass forces = passAt(400 * kMs);
    forces.forcesPlaying = true;
    control.afterPass(forces);
    EXPECT_EQ(control.deadlineNs(), 404 * kMs);
}

TEST(EventLoopControl, AfterWaitGivesTheReasonAndTheFloodGuard) {
    EventLoopControl control(true);
    control.afterPass(passAt(100 * kMs));
    EventLoopControl::Woke woke = control.afterWait(kGameFfb, kReasons, 100 * kMs + 200'000);
    EXPECT_EQ(woke.reason, WakeReason::GameFfb);
    EXPECT_EQ(woke.sleepNs, 300'000U);
    EXPECT_FALSE(woke.fellBack);
    EXPECT_EQ(control.wokeBy(), WakeReason::GameFfb);

    woke = control.afterWait(kWaitTimedOut, kReasons, 200 * kMs);
    EXPECT_EQ(woke.reason, WakeReason::Timer);
    EXPECT_EQ(woke.sleepNs, 0U);
    woke = control.afterWait(7, kReasons, 200 * kMs);   // an index nothing was added for
    EXPECT_EQ(woke.reason, WakeReason::Timer);
}

TEST(EventLoopControl, AFailedWaitFallsBackOnce) {
    EventLoopControl control(true);
    control.afterPass(passAt(0));
    EventLoopControl::Woke woke = control.afterWait(kWaitFailed, kReasons, 1 * kMs);
    EXPECT_TRUE(woke.fellBack);
    EXPECT_EQ(control.fallback(), LoopFallback::WaitFailed);
    EXPECT_EQ(woke.reason, WakeReason::Timer);
    woke = control.afterWait(kWaitFailed, kReasons, 2 * kMs);
    EXPECT_FALSE(woke.fellBack);   // already fixed: nothing new to log

    EventLoopControl fixed(false);
    EXPECT_FALSE(fixed.afterWait(kWaitFailed, kReasons, 0).fellBack);
    EXPECT_EQ(fixed.fallback(), LoopFallback::NotWanted);
}

TEST(EventLoopControl, TheFirstPassSaysNothingAboutNotifications) {
    EventLoopControl control(true);
    control.afterPass(passAt(0, true));   // the timer "woke" it: it ran before any wait
    (void)control.afterWait(kWaitTimedOut, kReasons, 100 * kMs);
    EXPECT_EQ(control.missedNotifications(), 0U);
}

TEST(EventLoopControl, ANotificationThatComesSoonAfterTheTimerPassWasLate) {
    EventLoopControl control(true);
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
    EventLoopControl control(true);
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 4 * kMs);
    control.afterPass(passAt(4 * kMs, true));
    (void)control.afterWait(kWheel, kReasons, 6 * kMs);   // a notification, but 2 ms later: another change's
    EXPECT_EQ(control.missedNotifications(), 1U);
    EXPECT_EQ(control.lateNotifications(), 0U);
}

TEST(EventLoopControl, TimerPassesWithoutNewInputCountForNothing) {
    EventLoopControl control(true);
    for (std::uint64_t t = 0; t < 10'000 * kMs; t += 100 * kMs) {
        control.afterPass(passAt(t));
        (void)control.afterWait(kWaitTimedOut, kReasons, t + 100 * kMs);
    }
    EXPECT_EQ(control.missedNotifications(), 0U);
    EXPECT_TRUE(control.events());
}

TEST(EventLoopControl, TwentyMissedWithinTheWindowFallBack) {
    EventLoopControl::Settings settings;
    settings.missThreshold = 3;
    EventLoopControl control(true, settings);
    control.afterPass(passAt(0));
    std::uint64_t t = 4 * kMs;
    bool fellBack = false;
    int waitsUntilFallback = 0;
    (void)control.afterWait(kWaitTimedOut, kReasons, t);
    while (!fellBack && waitsUntilFallback < 10) {
        control.afterPass(passAt(t, true));
        t += 4 * kMs;
        fellBack = control.afterWait(kWaitTimedOut, kReasons, t).fellBack;
        ++waitsUntilFallback;
    }
    EXPECT_TRUE(fellBack);
    EXPECT_EQ(waitsUntilFallback, 3);
    EXPECT_EQ(control.fallback(), LoopFallback::MissedNotifications);

    // Once fixed, nothing more is decided here.
    control.afterPass(passAt(t, true));
    EXPECT_FALSE(control.afterWait(kWaitTimedOut, kReasons, t + 4 * kMs).fellBack);
}

TEST(EventLoopControl, InFixedModeMissesAreNotCounted) {
    EventLoopControl control(false);
    control.afterPass(passAt(0));
    (void)control.afterWait(kWaitTimedOut, kReasons, 1 * kMs);
    control.afterPass(passAt(1 * kMs, true));
    (void)control.afterWait(kWaitTimedOut, kReasons, 5 * kMs);
    EXPECT_EQ(control.missedNotifications(), 0U);
}

}  // namespace
}  // namespace wheelbri
