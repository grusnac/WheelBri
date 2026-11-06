// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/ChangeCounter.hpp"

#include <algorithm>
#include <chrono>

namespace wheelbri {

namespace {
// A longer wait is cut to this (and counts as a timeout): a time near the clock's limit would overflow wait_for.
constexpr std::uint64_t kMaxWaitMs = 24ULL * 60 * 60 * 1000;
}  // namespace

void ChangeCounter::announce() {
    {
        std::lock_guard guard(mutex_);
        count_.fetch_add(1, std::memory_order_acq_rel);
    }
    changed_.notify_all();
}

bool ChangeCounter::waitForChange(const std::uint64_t seen, const std::uint64_t timeoutMs) {
    return waitForChange(seen, timeoutMs, wakes());
}

bool ChangeCounter::waitForChange(const std::uint64_t seen, const std::uint64_t timeoutMs,
                                  const std::uint64_t wakesSeen) {
    std::unique_lock guard(mutex_);
    changed_.wait_for(guard, std::chrono::milliseconds(std::min(timeoutMs, kMaxWaitMs)),
                      [&] { return count() != seen || wakeGeneration_ != wakesSeen; });
    return count() != seen;
}

std::uint64_t ChangeCounter::wakes() const {
    std::lock_guard guard(mutex_);
    return wakeGeneration_;
}

void ChangeCounter::wakeWaiters() {
    {
        std::lock_guard guard(mutex_);
        ++wakeGeneration_;
    }
    changed_.notify_all();
}

}  // namespace wheelbri
