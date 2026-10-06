#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <cstdint>

namespace wheelbri {

// How long before a deadline a thread that sleeps should wake up and spin the rest, learnt from how late its sleeps
// end. It rises at once after a late wake-up and falls back slowly, so one slow wake-up (a busy PC) does not make the
// thread spin through every wait from then on (until 2026-10-06 the 1 kHz loop kept the worst it had ever seen and
// spun about 97 % of each millisecond: one core busy for 25 us of work). Capped at kMaxNs: a tick that wakes later
// than that is late, which is cheaper than spinning for it every time.
class SpinMargin {
public:
    static constexpr std::uint64_t kMinNs = 50'000;
    static constexpr std::uint64_t kMaxNs = 300'000;
    static constexpr std::uint64_t kStartNs = 200'000;
    static constexpr std::uint64_t kHeadroomNs = 20'000;   // above the lateness seen
    static constexpr std::uint64_t kFallDivisor = 64;      // a 64th of the gap per sleep: ~1/4 s at 1 kHz

    [[nodiscard]] std::uint64_t ns() const { return marginNs_; }

    // A sleep ended `lateNs` after the time asked for (0 = on time).
    void observe(std::uint64_t lateNs);

private:
    std::uint64_t marginNs_ = kStartNs;
};

// Which ticks send the emulated wheel's state: only when it changed, at most once every `minIntervalNs`, and again
// after `keepAliveNs` without a change so receivers know the service is there (the shim and the driver call state
// older than 1500 ms stale). Every packet makes the virtual G920's driver complete a HID read (one thread of the
// driver host at 84 % of a core at 1000 a second, 2026-10-06) and the shim drain its socket, while games read ~90
// times a second. A change held back by the interval is sent at the first tick after it, never lost.
class StateSendGate {
public:
    static constexpr std::uint64_t kKeepAliveNs = 100'000'000;

    explicit StateSendGate(std::uint64_t minIntervalNs, std::uint64_t keepAliveNs = kKeepAliveNs)
        : minIntervalNs_(minIntervalNs), keepAliveNs_(keepAliveNs) {}

    // Whether `report` goes out at `nowNs`; true counts it as sent.
    [[nodiscard]] bool shouldSend(const TargetReport& report, std::uint64_t nowNs);

    [[nodiscard]] std::uint64_t sent() const { return sent_; }
    [[nodiscard]] std::uint64_t heldBack() const { return heldBack_; }   // ticks a change waited for the interval
    [[nodiscard]] std::uint64_t unchanged() const { return unchanged_; }

private:
    std::uint64_t minIntervalNs_;
    std::uint64_t keepAliveNs_;
    bool any_ = false;
    TargetReport last_;
    std::uint64_t lastSentNs_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t heldBack_ = 0;
    std::uint64_t unchanged_ = 0;
};

}  // namespace wheelbri
