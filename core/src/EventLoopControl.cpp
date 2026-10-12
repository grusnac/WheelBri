#include "wheelbri/EventLoopControl.hpp"

#include <algorithm>

namespace wheelbri {

const char* describe(const PollReason reason) {
    switch (reason) {
        case PollReason::None: return "notifications only";
        case PollReason::NoNotification: return "a device gives no notification of new input";
        case PollReason::SocketNotWaitable: return "a socket cannot be waited for";
        case PollReason::WaitFailed: return "the wait failed";
        case PollReason::MissedNotifications: return "a device's notifications miss its input";
    }
    return "unknown";
}

EventLoopControl::EventLoopControl(const Settings& settings)
    : settings_(settings), missed_(settings.missThreshold, settings.missWindowNs) {}

void EventLoopControl::pollBecause(const PollReason why) {
    if (pollReason_ == PollReason::None && why != PollReason::None) pollReason_ = why;
}

void EventLoopControl::afterPass(const Pass& pass) {
    state_.lastPassNs = pass.startNs;
    state_.windowOpen = pass.windowOpen;
    state_.forcesPlaying = pass.forcesPlaying;
    if (pass.inputChanged) {
        lastInputNs_ = pass.startNs;
        inputSeen_ = true;
    }
    state_.inputRecent = inputSeen_ && pass.startNs >= lastInputNs_ &&
                         pass.startNs - lastInputNs_ < settings_.wake.recentInputNs;
    // The first pass runs before any wait, so it says nothing about notifications. A timer pass that found new input:
    // the notification is missed unless it still comes, late (afterWait decides). Polling finds input on the timer by
    // design, so nothing is counted then.
    if (!polling() && waited_ && wokeBy_ == WakeReason::Timer && pass.inputChanged && !pendingMiss_) {
        pendingMiss_ = true;
        pendingMissNs_ = pass.startNs;
    }
}

std::uint64_t EventLoopControl::deadlineNs() const {
    const std::uint64_t planned = nextWakeNs(state_, settings_.wake);
    if (!polling()) return planned;
    return std::min(planned, state_.lastPassNs + std::max(settings_.pollNs, settings_.wake.minGapNs));
}

EventLoopControl::Woke EventLoopControl::afterWait(const int ready, const std::span<const WakeReason> reasons,
                                                   const std::uint64_t nowNs) {
    Woke woke;
    waited_ = true;
    if (ready == kWaitFailed) {
        woke.startedPolling = canWait();
        pollReason_ = PollReason::WaitFailed;   // over any earlier reason: the wait itself is gone now
        pendingMiss_ = false;
        wokeBy_ = WakeReason::Timer;
        return woke;
    }
    woke.reason = wakeReasonFor(ready, reasons);
    woke.sleepNs = floodGuardWaitNs(state_, settings_.wake, nowNs);
    wokeBy_ = woke.reason;

    if (pendingMiss_) {
        const bool inTime = nowNs >= pendingMissNs_ && nowNs - pendingMissNs_ <= settings_.lateToleranceNs;
        if (woke.reason == WakeReason::Wheel && inTime) {
            ++lateCount_;   // it came, a little after the state
            pendingMiss_ = false;
        } else if (!inTime) {
            ++missedCount_;
            pendingMiss_ = false;
            if (!polling() && missed_.record(WakeReason::Timer, true, pendingMissNs_)) {
                pollReason_ = PollReason::MissedNotifications;
                woke.startedPolling = true;
            }
        }
        // else: woken in time by something else (a datagram first): the notification may still come.
    }
    return woke;
}

}  // namespace wheelbri
