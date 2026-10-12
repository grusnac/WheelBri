#pragma once

#include "wheelbri/WakePlanner.hpp"

#include <cstdint>
#include <span>

namespace wheelbri {

// What platform::WaitSet::waitUntil returns when nothing was ready by the deadline, or the system could not wait
// (core has no OS headers).
inline constexpr int kWaitTimedOut = -1;
inline constexpr int kWaitFailed = -2;

// Why the loop polls (wakes every pollNs by itself) instead of relying on notifications alone.
enum class PollReason : std::uint8_t {
    None,                  // notifications only: the normal case
    NoNotification,        // a device DirectInput only polls (or whose notification could not be set up)
    SocketNotWaitable,     // a UDP socket the wait could not take
    WaitFailed,            // WaitForMultipleObjects / poll failed: from then on the loop sleeps instead of waiting
    MissedNotifications,   // the timer kept finding new input: the devices' notifications cannot be trusted
};

[[nodiscard]] const char* describe(PollReason reason);

// The decisions of the service's loop, without the OS: when to wake up by itself (WakePlanner), what woke it, the
// flood guard, and when to stop relying on notifications alone and poll as well. The loop waits for everything that
// can tell it about new input (platform::WaitSet: the devices' notifications, the game's force feedback and the test
// clutch sockets) and wakes by itself for what must run on time. Anything that cannot notify it, or notifications
// that miss input, make it also wake every pollNs (1 ms, the rate the loop ran at before it waited for events) for the
// rest of the session, so input never waits for idleNs. main.cpp's runLoop calls it around each pass and each wait;
// the tests run it against emulated devices (tests/emulators).
class EventLoopControl {
public:
    struct Settings {
        WakeSettings wake;
        std::uint64_t pollNs = 1'000'000;
        std::uint32_t missThreshold = NotificationCheck::kThreshold;
        std::uint64_t missWindowNs = NotificationCheck::kWindowNs;
        // A device's notification that comes this long after the timer's pass already read its new input was late,
        // not lost (the driver sets the event a moment after the state): it does not count as missed.
        std::uint64_t lateToleranceNs = 1'000'000;
    };

    EventLoopControl() : EventLoopControl(Settings{}) {}
    explicit EventLoopControl(const Settings& settings);

    // Setting up the wait found something that cannot notify the loop: it polls from the start (the first reason
    // stays).
    void pollBecause(PollReason why);

    [[nodiscard]] bool polling() const { return pollReason_ != PollReason::None; }
    [[nodiscard]] PollReason pollReason() const { return pollReason_; }
    // False once the wait itself failed: the loop then sleeps until deadlineNs() instead of waiting.
    [[nodiscard]] bool canWait() const { return pollReason_ != PollReason::WaitFailed; }
    [[nodiscard]] const Settings& settings() const { return settings_; }

    struct Pass {
        std::uint64_t startNs = 0;
        bool inputChanged = false;   // Pipeline::inputChanged()
        bool windowOpen = false;
        bool forcesPlaying = false;
    };
    // After a pass (before the wait that follows it).
    void afterPass(const Pass& pass);

    // When the wait after the last pass ends by itself at the latest.
    [[nodiscard]] std::uint64_t deadlineNs() const;

    struct Woke {
        WakeReason reason = WakeReason::Timer;   // what the next pass counts as its cause
        std::uint64_t sleepNs = 0;               // the flood guard: wait this long before the pass
        bool startedPolling = false;   // the loop polls from now on (pollReason() says why: WaitFailed or
                                       // MissedNotifications); log it once
    };
    // The wait's result (a source's index, kWaitTimedOut or kWaitFailed) at `nowNs`.
    Woke afterWait(int ready, std::span<const WakeReason> reasons, std::uint64_t nowNs);

    // The reason of the pass that runs now (set by afterWait; the timer before the first wait).
    [[nodiscard]] WakeReason wokeBy() const { return wokeBy_; }
    // Passes the timer woke that found new input no notification announced, not even late (0 with a healthy
    // device): notifications lost. Counted until the loop polls.
    [[nodiscard]] std::uint64_t missedNotifications() const { return missedCount_; }
    // Passes the timer woke that found new input whose notification came within lateToleranceNs after.
    [[nodiscard]] std::uint64_t lateNotifications() const { return lateCount_; }

private:
    Settings settings_;
    PollReason pollReason_ = PollReason::None;
    WakeState state_;
    NotificationCheck missed_;
    WakeReason wokeBy_ = WakeReason::Timer;
    std::uint64_t lastInputNs_ = 0;
    bool inputSeen_ = false;
    bool waited_ = false;   // the first pass runs before any wait: it says nothing about notifications
    std::uint64_t missedCount_ = 0;
    std::uint64_t lateCount_ = 0;
    bool pendingMiss_ = false;        // the last timer pass found new input; its notification may still come
    std::uint64_t pendingMissNs_ = 0;
};

}  // namespace wheelbri
