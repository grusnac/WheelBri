#include "wheelbri/WakePlanner.hpp"

#include <algorithm>
#include <cstdio>

namespace wheelbri {

std::uint64_t nextWakeNs(const WakeState& state, const WakeSettings& settings) {
    std::uint64_t interval = settings.idleNs;
    if (state.windowOpen) interval = std::min(interval, settings.windowNs);
    if (state.forcesPlaying) interval = std::min(interval, settings.ffbNs);
    if (state.inputRecent) interval = std::min(interval, settings.recentInputEveryNs);
    return state.lastPassNs + std::max(interval, settings.minGapNs);
}

std::uint64_t floodGuardWaitNs(const WakeState& state, const WakeSettings& settings, const std::uint64_t nowNs) {
    const std::uint64_t earliest = earliestPassNs(state, settings);
    return nowNs < earliest ? earliest - nowNs : 0;
}

WakeReason wakeReasonFor(const int ready, const std::span<const WakeReason> reasons) {
    if (ready < 0 || static_cast<std::size_t>(ready) >= reasons.size()) return WakeReason::Timer;
    return reasons[static_cast<std::size_t>(ready)];
}

bool Periodic::due(const std::uint64_t nowNs) {
    // A clock that went back (it should not) starts the interval again instead of firing at once.
    if (started_ && nowNs < lastNs_) {
        lastNs_ = nowNs;
        return false;
    }
    if (started_ && nowNs - lastNs_ < intervalNs_) return false;
    started_ = true;
    lastNs_ = nowNs;
    return true;
}

bool LossTimer::failed(const std::uint64_t nowMs) {
    if (failures_++ == 0 || nowMs < firstFailureMs_) firstFailureMs_ = nowMs;   // (a clock gone back: start again)
    return nowMs - firstFailureMs_ < lostAfterMs_;
}

std::uint64_t LossTimer::succeeded() {
    const std::uint64_t had = failures_;
    failures_ = 0;
    return had;
}

bool NotificationCheck::record(const WakeReason reason, const bool inputChanged, const std::uint64_t nowNs) {
    if (failed_) return false;
    if (missed_ == 0 || nowNs < windowStartNs_ || nowNs - windowStartNs_ >= windowNs_) {
        windowStartNs_ = nowNs;
        missed_ = 0;
    }
    if (reason != WakeReason::Timer || !inputChanged) return false;
    if (++missed_ < threshold_) return false;
    failed_ = true;
    return true;
}

void WakeStats::record(const WakeReason reason, const std::uint64_t workNs) {
    ++wakes_;
    ++counts_[static_cast<std::size_t>(reason)];
    workTotalNs_ += workNs;
    workMaxNs_ = std::max(workMaxNs_, workNs);
}

std::string WakeStats::describe(const std::uint64_t windowNs) const {
    const double seconds = static_cast<double>(windowNs) / 1e9;
    char text[256];
    std::snprintf(text, sizeof(text),
                  "%llu wake-ups in %.0f s (%.0f a second): wheel %llu, game force feedback %llu, test clutch %llu, "
                  "timer %llu; work avg %.1f us, max %.1f us",
                  static_cast<unsigned long long>(wakes_), seconds,
                  seconds > 0.0 ? static_cast<double>(wakes_) / seconds : 0.0,
                  static_cast<unsigned long long>(count(WakeReason::Wheel)),
                  static_cast<unsigned long long>(count(WakeReason::GameFfb)),
                  static_cast<unsigned long long>(count(WakeReason::TestClutch)),
                  static_cast<unsigned long long>(count(WakeReason::Timer)),
                  wakes_ > 0 ? static_cast<double>(workTotalNs_) / static_cast<double>(wakes_) / 1000.0 : 0.0,
                  static_cast<double>(workMaxNs_) / 1000.0);
    return text;
}

}  // namespace wheelbri
