#pragma once

#include "wheelbri/WakePlanner.hpp"

#include <cstdint>
#include <span>

namespace wheelbri {

// What platform::WaitSet::waitUntil returns when the system could not wait (core has no OS headers).
inline constexpr int kWaitTimedOut = -1;
inline constexpr int kWaitFailed = -2;

// Why a session that wanted loop_mode = events runs at the fixed rate.
enum class LoopFallback : std::uint8_t {
    None,
    NotWanted,             // loop_mode = fixed
    NoNotification,        // a device DirectInput only polls (or whose notification could not be set up)
    SocketNotWaitable,     // a UDP socket the wait could not take
    WaitFailed,            // WaitForMultipleObjects / poll failed during the session
    MissedNotifications,   // the timer kept finding new input: the device's notifications cannot be trusted
};

[[nodiscard]] const char* describe(LoopFallback fallback);

// The decisions of the service's loop in loop_mode = events, without the OS: when to wake up by itself (WakePlanner),
// what woke it, the flood guard, and when to give up on events for the rest of the session (fall back to the fixed
// rate). main.cpp's loop calls it around each pass and each wait; the tests run it against emulated devices.
class EventLoopControl {
public:
    struct Settings {
        WakeSettings wake;
        std::uint32_t missThreshold = NotificationCheck::kThreshold;
        std::uint64_t missWindowNs = NotificationCheck::kWindowNs;
        // A device's notification that comes this long after the timer's pass already read its new input was late,
        // not lost (the driver sets the event a moment after the state): it does not count as missed.
        std::uint64_t lateToleranceNs = 1'000'000;
    };

    explicit EventLoopControl(bool eventsWanted) : EventLoopControl(eventsWanted, Settings{}) {}
    EventLoopControl(bool eventsWanted, const Settings& settings);

    // Setting up the wait found something it cannot wait for: the session runs at the fixed rate (the first reason
    // stays).
    void setupFailed(LoopFallback why);

    [[nodiscard]] bool events() const { return fallback_ == LoopFallback::None; }
    [[nodiscard]] LoopFallback fallback() const { return fallback_; }
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
    [[nodiscard]] std::uint64_t deadlineNs() const { return nextWakeNs(state_, settings_.wake); }

    struct Woke {
        WakeReason reason = WakeReason::Timer;   // what the next pass counts as its cause
        std::uint64_t sleepNs = 0;               // the flood guard: wait this long before the pass
        bool fellBack = false;   // the loop gives up on events now (fallback() says why: WaitFailed or
                                 // MissedNotifications); log it once and run at the fixed rate
    };
    // The wait's result (a source's index, kWaitTimedOut or kWaitFailed) at `nowNs`.
    Woke afterWait(int ready, std::span<const WakeReason> reasons, std::uint64_t nowNs);

    // The reason of the pass that runs now (set by afterWait; the timer before the first wait).
    [[nodiscard]] WakeReason wokeBy() const { return wokeBy_; }
    // Passes the timer woke that found new input no notification announced, not even late (0 with a healthy
    // device): notifications lost.
    [[nodiscard]] std::uint64_t missedNotifications() const { return missedCount_; }
    // Passes the timer woke that found new input whose notification came within lateToleranceNs after.
    [[nodiscard]] std::uint64_t lateNotifications() const { return lateCount_; }

private:
    Settings settings_;
    LoopFallback fallback_ = LoopFallback::None;
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
