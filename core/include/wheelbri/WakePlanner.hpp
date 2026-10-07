#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace wheelbri {

// When the service's loop wakes up by itself in the events mode (`[emulator] loop_mode = events`): between those
// wake-ups it sleeps until Windows (or Linux) says the wheel, an accessory or the game's force feedback has something
// new. The timer is only for what must happen on time without a new event:
//   - force feedback while forces play: every `ffbNs` (FfbController's minUpdateNs, 4 ms), so the safety limiter's
//     slew steps stay smooth;
//   - the window's copy of the state while one is open: every `windowNs` (~30 a second);
//   - everything else (the 100 ms keep-alive to the game, the 500 ms force feedback watchdog, the test clutch
//     running out, statistics, telemetry heartbeats, Stop): at least every `idleNs`.
// And never two passes closer than `minGapNs`, so a device or socket that signals without pause cannot spin the loop.
struct WakeSettings {
    std::uint64_t idleNs = 100'000'000;
    std::uint64_t windowNs = 33'333'333;
    std::uint64_t ffbNs = 4'000'000;
    std::uint64_t minGapNs = 500'000;
};

struct WakeState {
    std::uint64_t lastPassNs = 0;   // when the last pass of the loop started
    bool windowOpen = false;
    bool forcesPlaying = false;     // any force feedback effect playing (or wanted) now
};

// The time (same clock as lastPassNs) the loop wakes up by itself at the latest.
[[nodiscard]] std::uint64_t nextWakeNs(const WakeState& state, const WakeSettings& settings);

// The earliest the next pass may start (the flood guard): a wake-up before it waits until then.
[[nodiscard]] inline std::uint64_t earliestPassNs(const WakeState& state, const WakeSettings& settings) {
    return state.lastPassNs + settings.minGapNs;
}

// How long a pass that woke at `nowNs` waits before it runs (0: at once): what is left of minGapNs since the last pass.
[[nodiscard]] std::uint64_t floodGuardWaitNs(const WakeState& state, const WakeSettings& settings, std::uint64_t nowNs);

// What woke the loop, counted for the log ("loop (events): ...").
enum class WakeReason : std::uint8_t { Wheel, GameFfb, TestClutch, Timer, kCount };

// What platform::WaitSet's result means: the reason registered for that source (`reasons[ready]`), else the timer
// (ready < 0: the deadline; an index nothing was registered for counts as the timer too, never out of bounds).
[[nodiscard]] WakeReason wakeReasonFor(int ready, std::span<const WakeReason> reasons);

// Something to do every `intervalNs` by the clock (the window's copy, --print, the wake-up log), however often the
// loop passes: due() is true at the first call and then once `intervalNs` has passed since the last time it was.
class Periodic {
public:
    explicit Periodic(std::uint64_t intervalNs) : intervalNs_(intervalNs) {}
    [[nodiscard]] bool due(std::uint64_t nowNs);
    [[nodiscard]] std::uint64_t intervalNs() const { return intervalNs_; }

private:
    std::uint64_t intervalNs_;
    std::uint64_t lastNs_ = 0;
    bool started_ = false;
};

// A device that stopped answering is gone after `lostAfterMs` of failed reads by the clock, not after a number of
// reads: the events loop reads a few times a second at rest, the fixed one 1000 (DinputReader; before 2026-10-07 it
// counted 500 reads, which in the events loop would have been a minute).
class LossTimer {
public:
    static constexpr std::uint64_t kLostAfterMs = 500;
    explicit LossTimer(std::uint64_t lostAfterMs = kLostAfterMs) : lostAfterMs_(lostAfterMs) {}

    // A read failed at `nowMs`: true while the device still counts as there.
    [[nodiscard]] bool failed(std::uint64_t nowMs);
    // A read worked: the failures so far are forgotten; how many there were (0: none).
    std::uint64_t succeeded();
    [[nodiscard]] std::uint64_t failures() const { return failures_; }

private:
    std::uint64_t lostAfterMs_;
    std::uint64_t failures_ = 0;
    std::uint64_t firstFailureMs_ = 0;
};

// Whether the devices' notifications can be trusted. The events loop reads the wheel at every pass; a pass the timer
// woke that still finds new input means that input arrived without a notification (a driver that does not set
// DirectInput's event, or a device that stopped). Once that happens `threshold` times within `windowNs`, record()
// says so (once), and the loop goes back to the fixed rate. A lone one can be a race (input arriving just after the
// timer woke the loop), so it takes several.
class NotificationCheck {
public:
    static constexpr std::uint32_t kThreshold = 20;
    static constexpr std::uint64_t kWindowNs = 10'000'000'000ULL;
    explicit NotificationCheck(std::uint32_t threshold = kThreshold, std::uint64_t windowNs = kWindowNs)
        : threshold_(threshold), windowNs_(windowNs) {}

    // One pass: what woke it and whether its read found new input. True once: the notifications are missing input.
    [[nodiscard]] bool record(WakeReason reason, bool inputChanged, std::uint64_t nowNs);
    [[nodiscard]] std::uint32_t missed() const { return missed_; }   // in the current window
    [[nodiscard]] bool failed() const { return failed_; }

private:
    std::uint32_t threshold_;
    std::uint64_t windowNs_;
    std::uint64_t windowStartNs_ = 0;
    std::uint32_t missed_ = 0;
    bool failed_ = false;
};

class WakeStats {
public:
    void record(WakeReason reason, std::uint64_t workNs);
    [[nodiscard]] std::uint64_t wakes() const { return wakes_; }
    [[nodiscard]] std::uint64_t count(WakeReason reason) const { return counts_[static_cast<std::size_t>(reason)]; }
    // "6012 wake-ups in 60 s (100 a second): wheel 5400, game force feedback 600, timer 12; work avg 21 us, max 300 us"
    [[nodiscard]] std::string describe(std::uint64_t windowNs) const;
    void reset() { *this = WakeStats{}; }

private:
    std::uint64_t wakes_ = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(WakeReason::kCount)> counts_{};
    std::uint64_t workTotalNs_ = 0;
    std::uint64_t workMaxNs_ = 0;
};

}  // namespace wheelbri
