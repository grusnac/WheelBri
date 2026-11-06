// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/InputReadStats.hpp"
#include "wheelbri/SenderGate.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace wheelbri::shim {

// The emulated wheel's state as published by wheelbri-service over UDP loopback. One per process,
// shared by every emulated device. Never blocks: each call drains whatever packets have arrived.
class StateFeed {
public:
    [[nodiscard]] static StateFeed& instance();

    // Latest report from the service. If nothing arrived for `stale_after_ms` (service not running,
    // crashed, or the wheel was unplugged) this is the profile's neutral report: wheel centred, pedals
    // released, no buttons. A stuck throttle is worse than no input.
    [[nodiscard]] TargetReport current(const IDeviceProfile& profile);

    // One read by the game (GetDeviceState), its start and end on the steady clock in ns: how often the game reads,
    // whether each read had new state and how long the shim took (InputReadStats; a log line a minute).
    void noteRead(std::uint64_t startNs, std::uint64_t endNs);
    // The reads since the last line, now (the device is released).
    void logReads();

private:
    StateFeed();
    void openReceiver(std::uint16_t port);

    std::mutex mutex_;
    bool opened_ = false;
    std::chrono::steady_clock::time_point lastOpenAttempt_{};
    std::unique_ptr<UdpLoopbackReceiver> receiver_;
    // Only WheelBri may feed the wheel's state; under Wine (the service a Linux program) nobody can tell: let through.
    SenderGate gate_;
    std::optional<StatePacket> latest_;
    std::chrono::steady_clock::time_point lastArrival_{};
    bool stale_ = true;
    bool warnedNeverReceived_ = false;
    std::chrono::steady_clock::time_point firstRequest_{};
    InputReadStats reads_;
};

}  // namespace wheelbri::shim
