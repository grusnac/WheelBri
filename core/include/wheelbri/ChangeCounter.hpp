#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace wheelbri {

// Counts "something changed" announcements (game controllers coming or going: the service's platform::DeviceChanges
// announces from the system's threads) and lets other threads wait for the next one. It never says what changed: a
// waiter compares count() with the value it last saw and looks for itself. Thread safe.
class ChangeCounter {
public:
    // One more change; wakes every waiter.
    void announce();
    // Changes announced so far (only ever grows).
    [[nodiscard]] std::uint64_t count() const { return count_.load(std::memory_order_acquire); }
    // Waits until count() is no longer `seen` (at once when it already is not), `timeoutMs` passed, or wakeWaiters()
    // was called after the wait began. True when it changed. A timeout over a day is cut to a day.
    bool waitForChange(std::uint64_t seen, std::uint64_t timeoutMs);
    // The same, but a wakeWaiters() since `wakesSeen` (a value of wakes()) also ends it, at once if it came before the
    // wait began: a waiter reads wakes() when it looks at its own state, so a wake that comes after that look is never
    // lost (DeviceWatch: switched off and on again while it was listing).
    bool waitForChange(std::uint64_t seen, std::uint64_t timeoutMs, std::uint64_t wakesSeen);
    // Ends every wait in progress now, without a change (a waiter that is stopping or switching off); a wait that
    // begins afterwards is ended only through its `wakesSeen`.
    void wakeWaiters();
    // How many times wakeWaiters() was called.
    [[nodiscard]] std::uint64_t wakes() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<std::uint64_t> count_{0};
    std::uint64_t wakeGeneration_ = 0;   // under mutex_
};

}  // namespace wheelbri
