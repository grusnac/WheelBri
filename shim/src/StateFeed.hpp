#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/UdpLoopback.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace wheelemu::shim {

// The emulated wheel's state as published by wheelemu-service over UDP loopback. One per process,
// shared by every emulated device. Never blocks: each call drains whatever packets have arrived.
class StateFeed {
public:
    [[nodiscard]] static StateFeed& instance();

    // Latest report from the service. If nothing arrived for `stale_after_ms` (service not running,
    // crashed, or the wheel was unplugged) this is the profile's neutral report: wheel centred, pedals
    // released, no buttons. A stuck throttle is worse than no input.
    [[nodiscard]] TargetReport current(const IDeviceProfile& profile);

private:
    StateFeed() = default;

    std::mutex mutex_;
    bool opened_ = false;
    std::unique_ptr<UdpLoopbackReceiver> receiver_;
    std::optional<StatePacket> latest_;
    std::chrono::steady_clock::time_point lastArrival_{};
    bool stale_ = true;
    bool warnedNeverReceived_ = false;
    std::chrono::steady_clock::time_point firstRequest_{};
};

}  // namespace wheelemu::shim
