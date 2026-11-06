// SPDX-License-Identifier: GPL-3.0-or-later
#include "Wheel.hpp"

#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/Logging.hpp"

#include <string>


namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

}  // namespace

std::unique_ptr<Wheel> openWheel(WheelBackend& backend, const std::string& match, const bool quiet) {
    const std::vector<WheelInfo> wheels = backend.list();
    bool found = false;
    for (const WheelInfo& info : wheels) {
        const bool matches = deviceMatches(info.name, info.vendorId, info.productId, match);
        if (!quiet) {
            WE_LOG_DEBUG(log(), "candidate '{}' ({:04x}:{:04x}) {}", info.name, info.vendorId, info.productId,
                         matches ? "matches" : "skipped");
        }
        if (!matches) {
            continue;
        }
        found = true;
        if (auto wheel = backend.open(info)) {
            WE_LOG_INFO(log(), "opened '{}' ({:04x}:{:04x}) through {}: axes={} buttons={} hats={} path={}", info.name,
                        info.vendorId, info.productId, backend.describe(), wheel->axes(), wheel->buttons(),
                        wheel->hats(), info.path);
            if (wheel->axes() > static_cast<int>(RawInputState::kMaxAxes)) {
                WE_LOG_WARN(log(), "device has {} axes, only the first {} are read", wheel->axes(),
                            RawInputState::kMaxAxes);
            }
            return wheel;
        }
    }
    // Retried about once a second while waiting for a lost wheel (`quiet`): then a line every 30 s at most.
    static logging::RateLimiter limiter(std::chrono::seconds(30));
    std::uint64_t suppressed = 0;
    if (quiet && !limiter.allow(suppressed)) return nullptr;
    const std::string more = suppressed > 0 ? " (" + std::to_string(suppressed) + " similar suppressed)" : "";
    if (found) {
        WE_LOG_ERROR(log(), "'{}' is connected but cannot be opened (another program may hold it){}", match, more);
    } else {
        WE_LOG_ERROR(log(), "no game controller name or vid:pid contains '{}' ({} connected); check [emulator] "
                            "source_device{}",
                     match, wheels.size(), more);
    }
    return nullptr;
}

bool wheelConnected(WheelBackend& backend, const std::string& match) {
    for (const WheelInfo& info : backend.list()) {
        if (deviceMatches(info.name, info.vendorId, info.productId, match)) return true;
    }
    return false;
}

}  // namespace wheelbri::service
