// The service loop end to end on emulated devices (tests/emulators): its decisions (core
// EventLoopControl) and the real Pipeline, on a virtual clock, against devices that push their input the way
// DirectInput and evdev do, and against devices that do it badly.

#include "emulators/PushEmulators.hpp"

#include <gtest/gtest.h>

namespace wheelbri::emulators {
namespace {

constexpr std::uint64_t kS = 1'000'000'000;
const WakeSettings kWake;

using Behaviour = EmulatedPushDevice::Behaviour;

// --- a healthy device ---

TEST(PushLoop, WhileDrivingEveryChangeIsReadWhenItIsAnnounced) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    wheel.sweep(0, 2 * kS, 1 * kMs);   // a wheel reporting 1000 times a second
    PushLoopRig rig(clock, wheel);
    rig.run(2 * kS);

    EXPECT_FALSE(rig.control().polling());
    EXPECT_EQ(rig.maxLatency(wheel), 0U);
    EXPECT_EQ(rig.control().missedNotifications(), 0U);
    EXPECT_EQ(rig.control().lateNotifications(), 0U);
    EXPECT_GE(rig.wokenBy(WakeReason::Wheel), 1999U);
}

TEST(PushLoop, AtRestOnlyTheIdleTimerWakesIt) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    PushLoopRig rig(clock, wheel);
    rig.run(2 * kS);

    EXPECT_FALSE(rig.control().polling());
    EXPECT_EQ(rig.passes(), 2 * kS / kWake.idleNs);   // 0, 100, ..., 1900 ms
    EXPECT_EQ(rig.wokenBy(WakeReason::Wheel), 0U);
}

TEST(PushLoop, NotificationsWithoutAChangeCostPassesButNeverAFallback) {
    VirtualClock clock;
    Behaviour r3AtRest;
    r3AtRest.spuriousEveryNs = 2'200 * kUs;   // the R3 at rest: ~450 a second
    EmulatedPushDevice wheel("R3", clock, r3AtRest);
    PushLoopRig rig(clock, wheel);
    rig.run(1 * kS);

    EXPECT_FALSE(rig.control().polling());
    EXPECT_NEAR(static_cast<double>(rig.passes()), 455.0, 5.0);
    EXPECT_EQ(rig.control().missedNotifications(), 0U);
}

TEST(PushLoop, ADeviceThatSignalsWithoutPauseIsHeldToTheMinimumGap) {
    VirtualClock clock;
    Behaviour flood;
    flood.spuriousEveryNs = 10 * kUs;
    EmulatedPushDevice wheel("R3", clock, flood);
    PushLoopRig rig(clock, wheel);
    rig.run(1 * kS);

    EXPECT_GE(rig.minPassGap(), kWake.minGapNs);
    EXPECT_LE(rig.passes(), kS / kWake.minGapNs + 1);
    EXPECT_FALSE(rig.control().polling());
}

TEST(PushLoop, AnAccessoryWakesTheLoopToo) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    EmulatedPushDevice pedals("pedals", clock);
    for (std::uint64_t t = 10 * kMs; t < 1 * kS; t += 37 * kMs) pedals.moveAxis(t, 1, static_cast<float>(t % 7) / 7.0F);
    PushLoopRig rig(clock, wheel);
    rig.addAccessory(pedals);
    rig.run(1 * kS);

    EXPECT_FALSE(rig.control().polling());
    EXPECT_EQ(rig.maxLatency(pedals), 0U);
    EXPECT_EQ(rig.wokenBy(WakeReason::Wheel), pedals.changeTimes().size());
}

TEST(PushLoop, GameForceFeedbackWakesTheLoop) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    EmulatedSocket ffb;
    ffb.arriveEvery(5 * kMs, 1 * kS, 28 * kMs);   // NFS: ~35 changes a second
    PushLoopRig rig(clock, wheel);
    rig.addGameFfb(ffb);
    rig.run(1 * kS);

    ASSERT_EQ(ffb.latencies().size(), 36U);
    for (const std::uint64_t latency : ffb.latencies()) EXPECT_EQ(latency, 0U);
    EXPECT_EQ(rig.wokenBy(WakeReason::GameFfb), 36U);
}

TEST(PushLoop, AFloodOnTheForceFeedbackPortIsHeldToTheGapAndTheGameStillGetsThrough) {
    // Any local program can send to 127.0.0.1:41330. A datagram every 10 us wakes the loop at most every 0.5 ms
    // (2000 passes a second, twice what polling every millisecond costs); the game's own packets are still read
    // within that gap, and the wheel's input too.
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    wheel.sweep(0, 1 * kS, 3 * kMs);
    EmulatedSocket ffb;
    ffb.arriveEvery(0, 1 * kS, 10 * kUs);
    PushLoopRig rig(clock, wheel);
    rig.addGameFfb(ffb);
    rig.run(1 * kS);

    EXPECT_FALSE(rig.control().polling());
    EXPECT_GE(rig.minPassGap(), kWake.minGapNs);
    EXPECT_LE(rig.passes(), kS / kWake.minGapNs + 1);
    for (const std::uint64_t latency : ffb.latencies()) EXPECT_LE(latency, kWake.minGapNs + 50 * kUs);
    EXPECT_LE(rig.maxLatency(wheel), kWake.minGapNs + 50 * kUs);
}

TEST(PushLoop, WhatMustRunOnTimeSetsTheTimer) {
    {
        VirtualClock clock;
        EmulatedPushDevice wheel("R3", clock);
        PushLoopRig::Options options;
        options.forcesPlaying = true;   // every 4 ms
        PushLoopRig rig(clock, wheel, options);
        rig.run(1 * kS);
        EXPECT_EQ(rig.passes(), 250U);
    }
    {
        VirtualClock clock;
        EmulatedPushDevice wheel("R3", clock);
        PushLoopRig::Options options;
        options.windowOpen = true;   // 60 a second
        PushLoopRig rig(clock, wheel, options);
        rig.run(1 * kS);
        EXPECT_EQ(rig.passes(), 61U);
    }
}

TEST(PushLoop, AfterNewInputTheTimerWakesEvery4msForAWhile) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    wheel.moveAxis(500 * kMs, 0, 0.5F);
    PushLoopRig rig(clock, wheel);
    rig.run(1 * kS);

    // 100 ms of 4 ms wake-ups after the change, then the idle timer again.
    EXPECT_EQ(rig.passesBetween(500 * kMs, 600 * kMs), 25U);   // the change's own pass and 24 by the timer
    EXPECT_LE(rig.passesBetween(600 * kMs, 1 * kS), 5U);
}

// --- a device whose notifications go wrong ---

TEST(PushLoop, ANotificationLostNowAndThenWhileTurningCostsAt4msMost) {
    VirtualClock clock;
    Behaviour lossy;
    lossy.dropEvery = 10;   // one change in ten announces nothing
    EmulatedPushDevice wheel("R3", clock, lossy);
    wheel.sweep(0, 3 * kS, 21 * kMs);   // turned slowly
    PushLoopRig rig(clock, wheel);
    rig.run(3 * kS);

    EXPECT_FALSE(rig.control().polling());   // 14 lost in 3 s: below 20 in 10 s
    EXPECT_EQ(rig.control().missedNotifications(), wheel.droppedNotifications());
    EXPECT_LE(rig.maxLatency(wheel), kWake.recentInputEveryNs);
    EXPECT_GT(rig.maxLatency(wheel), 0U);
}

TEST(PushLoop, LosingOneInTenForLongStartsPolling) {
    VirtualClock clock;
    Behaviour lossy;
    lossy.dropEvery = 10;
    EmulatedPushDevice wheel("R3", clock, lossy);
    wheel.sweep(0, 10 * kS, 21 * kMs);
    PushLoopRig rig(clock, wheel);
    rig.run(10 * kS);

    EXPECT_EQ(rig.control().pollReason(), PollReason::MissedNotifications);
    ASSERT_TRUE(rig.pollingFromNs().has_value());
    EXPECT_NEAR(static_cast<double>(*rig.pollingFromNs()) / kS, 4.2, 0.1);   // the 20th lost one
    EXPECT_LE(rig.maxLatency(wheel, *rig.pollingFromNs()), 1 * kMs);   // then every millisecond
}

TEST(PushLoop, NotificationsThatStopWhileDrivingStartPollingWithin100ms) {
    VirtualClock clock;
    Behaviour stops;
    stops.stopNotifyingAtNs = 1 * kS;
    EmulatedPushDevice wheel("R3", clock, stops);
    wheel.sweep(0, 2 * kS, 1 * kMs);
    PushLoopRig rig(clock, wheel);
    rig.run(2 * kS);

    EXPECT_EQ(rig.control().pollReason(), PollReason::MissedNotifications);
    ASSERT_TRUE(rig.pollingFromNs().has_value());
    EXPECT_GT(*rig.pollingFromNs(), 1 * kS);
    EXPECT_LT(*rig.pollingFromNs(), 1 * kS + 100 * kMs);
    EXPECT_LE(rig.maxLatency(wheel), kWake.recentInputEveryNs);   // even before it polls
    EXPECT_LE(rig.maxLatency(wheel, *rig.pollingFromNs()), 1 * kMs);
}

TEST(PushLoop, LateNotificationsAreNotTakenForLostOnes) {
    VirtualClock clock;
    Behaviour late;
    late.notifyDelayNs = 500 * kUs;   // the event comes half a millisecond after the state
    EmulatedPushDevice wheel("R3", clock, late);
    wheel.sweep(0, 3 * kS, 4'300 * kUs);   // so the 4 ms timer often reads a change before its notification
    PushLoopRig rig(clock, wheel);
    rig.run(3 * kS);

    EXPECT_FALSE(rig.control().polling());
    EXPECT_EQ(rig.control().missedNotifications(), 0U);
    EXPECT_GT(rig.control().lateNotifications(), 0U);
    EXPECT_LE(rig.maxLatency(wheel), late.notifyDelayNs);
}

TEST(PushLoop, ALostNotificationAtRestWaitsForTheIdleTimer) {
    // The limit of the events mode, measured: a single change that announces nothing while nothing else happens is
    // read by the next idle wake-up, up to idleNs later. The game reads it a little late; nothing is lost.
    VirtualClock clock;
    Behaviour lost;
    lost.dropEvery = 1;
    EmulatedPushDevice wheel("R3", clock, lost);
    wheel.setButton(1'050 * kMs, 3, true);
    PushLoopRig rig(clock, wheel);
    rig.run(2 * kS);

    EXPECT_EQ(rig.maxLatency(wheel), 50 * kMs);   // the 1100 ms wake-up
    EXPECT_LE(rig.maxLatency(wheel), kWake.idleNs);
    EXPECT_EQ(rig.control().missedNotifications(), 1U);
    EXPECT_FALSE(rig.control().polling());
}

// --- what cannot be waited for ---

TEST(PushLoop, AWheelDirectInputOnlyPollsIsPolledEveryMillisecond) {
    VirtualClock clock;
    Behaviour polled;
    polled.notifies = false;
    EmulatedPushDevice wheel("polled wheel", clock, polled);
    wheel.sweep(0, 1 * kS, 3 * kMs);
    PushLoopRig rig(clock, wheel);
    rig.run(1 * kS);

    EXPECT_EQ(rig.control().pollReason(), PollReason::NoNotification);
    EXPECT_EQ(rig.passes(), 1000U);
    EXPECT_LE(rig.maxLatency(wheel), 1 * kMs);
}

TEST(PushLoop, APolledAccessoryIsPolledAndTheWheelStillWakesTheLoop) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    wheel.sweep(0, 1 * kS, 3'300 * kUs);   // never on the millisecond: only its notification reads it at once
    Behaviour polled;
    polled.notifies = false;
    EmulatedPushDevice pedals("polled pedals", clock, polled);
    pedals.moveAxis(300 * kMs, 1, 0.7F);
    PushLoopRig rig(clock, wheel);
    rig.addAccessory(pedals);
    rig.run(1 * kS);

    EXPECT_EQ(rig.control().pollReason(), PollReason::NoNotification);
    EXPECT_LE(rig.maxLatency(pedals), 1 * kMs);
    EXPECT_LE(rig.maxLatency(wheel, 0, 990 * kMs), kWake.minGapNs);   // by its own notification, not the 1 ms poll
    EXPECT_GT(rig.wokenBy(WakeReason::Wheel), 0U);
}

TEST(PushLoop, ASocketTheWaitCannotTakeIsReadEveryMillisecond) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    wheel.sweep(0, 1 * kS, 3'300 * kUs);
    EmulatedSocket ffb(false);
    ffb.arriveEvery(0, 1 * kS, 28 * kMs);
    PushLoopRig rig(clock, wheel);
    rig.addGameFfb(ffb);
    rig.run(1 * kS);

    EXPECT_EQ(rig.control().pollReason(), PollReason::SocketNotWaitable);
    for (const std::uint64_t latency : ffb.latencies()) EXPECT_LE(latency, 1 * kMs);
    EXPECT_LE(rig.maxLatency(wheel, 0, 990 * kMs), kWake.minGapNs);   // the wheel still wakes it
}

TEST(PushLoop, AWaitThatFailsMidSessionPollsAtOnceAndStopsWaiting) {
    VirtualClock clock;
    EmulatedPushDevice wheel("R3", clock);
    wheel.sweep(0, 1 * kS, 7 * kMs);
    PushLoopRig rig(clock, wheel);
    rig.waitSet().failFrom(400 * kMs);
    rig.run(1 * kS);

    EXPECT_EQ(rig.control().pollReason(), PollReason::WaitFailed);
    ASSERT_TRUE(rig.pollingFromNs().has_value());
    EXPECT_GE(*rig.pollingFromNs(), 400 * kMs);
    EXPECT_LT(*rig.pollingFromNs(), 401 * kMs);
    EXPECT_EQ(rig.passesBetween(500 * kMs, 1 * kS), 500U);   // every millisecond from then on
    EXPECT_LE(rig.maxLatency(wheel), 1 * kMs);
    EXPECT_FALSE(rig.control().canWait());
    const std::uint64_t waits = rig.waitSet().waits();
    EXPECT_LE(waits, 200U);   // the waits before 400 ms and the one that failed: none after it
}

// --- a device that goes away ---

TEST(PushLoop, AnUnpluggedWheelThatSignalsIsFoundAtOnce) {
    VirtualClock clock;
    Behaviour unplugged;
    unplugged.disconnectAtNs = 1'234 * kMs;
    EmulatedPushDevice wheel("R3", clock, unplugged);
    PushLoopRig rig(clock, wheel);
    rig.run(2 * kS);

    ASSERT_TRUE(rig.lostAtNs().has_value());
    EXPECT_EQ(*rig.lostAtNs(), 1'234 * kMs);
}

TEST(PushLoop, AnUnpluggedWheelThatSaysNothingIsFoundByTheIdleTimer) {
    VirtualClock clock;
    Behaviour unplugged;
    unplugged.disconnectAtNs = 1'234 * kMs;
    unplugged.signalsDisconnect = false;
    EmulatedPushDevice wheel("R3", clock, unplugged);
    PushLoopRig rig(clock, wheel);
    rig.run(2 * kS);

    ASSERT_TRUE(rig.lostAtNs().has_value());
    EXPECT_EQ(*rig.lostAtNs(), 1'300 * kMs);
}

}  // namespace
}  // namespace wheelbri::emulators
