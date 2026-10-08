#include "wheelbri/RelistTimer.hpp"

#include <algorithm>

namespace wheelbri {

bool RelistTimer::due(const std::uint64_t nowMs, const std::uint64_t changes) {
    if (!listedOnce_) return true;
    if (changes != listedChanges_ && !changePending_) {
        changePending_ = true;
        changeSeenMs_ = nowMs;
    }
    // A clock that went back (it should not) counts as due rather than waiting for ever.
    if (nowMs < lastListMs_) return true;
    if (changePending_ && nowMs - changeSeenMs_ >= settings_.settleMs) return true;
    if (followUpPending_ && nowMs >= followUpMs_) return true;
    return nowMs - lastListMs_ >= settings_.safetyMs;
}

void RelistTimer::listed(const std::uint64_t nowMs, const std::uint64_t changes) {
    // A listing caused by a change gets one more a little later; the follow-up itself does not get another.
    const bool byChange = listedOnce_ && (changePending_ || changes != listedChanges_);
    listedOnce_ = true;
    lastListMs_ = nowMs;
    listedChanges_ = changes;
    changePending_ = false;
    followUpPending_ = byChange;
    followUpMs_ = nowMs + settings_.followUpMs;
}

std::uint64_t RelistTimer::nextDueMs(const std::uint64_t nowMs, const std::uint64_t changes) const {
    if (!listedOnce_ || nowMs < lastListMs_) return nowMs;   // as due(): the first time, or a clock gone back
    std::uint64_t next = lastListMs_ + settings_.safetyMs;
    if (changePending_) {
        next = std::min(next, changeSeenMs_ + settings_.settleMs);
    } else if (changes != listedChanges_) {
        next = std::min(next, nowMs + settings_.settleMs);   // due() notices it at the next call
    }
    if (followUpPending_) next = std::min(next, followUpMs_);
    return std::max(next, nowMs);
}

}  // namespace wheelbri
