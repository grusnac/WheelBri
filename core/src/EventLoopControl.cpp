#include "wheelbri/EventLoopControl.hpp"

namespace wheelbri {

const char* describe(const LoopFallback fallback) {
    switch (fallback) {
        case LoopFallback::None: return "running on events";
        case LoopFallback::NotWanted: return "loop_mode = fixed";
        case LoopFallback::NoNotification: return "a device gives no notification of new input";
        case LoopFallback::SocketNotWaitable: return "a socket cannot be waited for";
        case LoopFallback::WaitFailed: return "the wait failed";
        case LoopFallback::MissedNotifications: return "a device's notifications miss its input";
    }
    return "unknown";
}

EventLoopControl::EventLoopControl(const bool eventsWanted, const Settings& settings)
    : settings_(settings),
      fallback_(eventsWanted ? LoopFallback::None : LoopFallback::NotWanted),
      missed_(settings.missThreshold, settings.missWindowNs) {}

void EventLoopControl::setupFailed(const LoopFallback why) {
    if (fallback_ == LoopFallback::None && why != LoopFallback::None) fallback_ = why;
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
    // the notification is missed unless it still comes, late (afterWait decides).
    if (events() && waited_ && wokeBy_ == WakeReason::Timer && pass.inputChanged && !pendingMiss_) {
        pendingMiss_ = true;
        pendingMissNs_ = pass.startNs;
    }
}

EventLoopControl::Woke EventLoopControl::afterWait(const int ready, const std::span<const WakeReason> reasons,
                                                   const std::uint64_t nowNs) {
    Woke woke;
    waited_ = true;
    if (ready == kWaitFailed) {
        woke.fellBack = events();
        if (woke.fellBack) fallback_ = LoopFallback::WaitFailed;
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
            if (events() && missed_.record(WakeReason::Timer, true, pendingMissNs_)) {
                fallback_ = LoopFallback::MissedNotifications;
                woke.fellBack = true;
            }
        }
        // else: woken in time by something else (a datagram first): the notification may still come.
    }
    return woke;
}

}  // namespace wheelbri
