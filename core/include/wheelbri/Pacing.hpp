#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <cstdint>

namespace wheelbri {

// Which passes of the loop send the emulated wheel's state: only when it changed, at most once every `minIntervalNs`,
// and again after `keepAliveNs` without a change so receivers know the service is there (the shim and the driver call
// state older than 1500 ms stale). Every packet costs the virtual G920's driver one HID read and the shim a socket
// read, while games read ~90 times a second. A change held back by the interval is sent at the first pass after it,
// never lost.
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
