#include "Wheel.hpp"

#include "wheelemu/DeviceMatch.hpp"
#include "wheelemu/Logging.hpp"


namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

}  // namespace

std::unique_ptr<Wheel> openWheel(WheelBackend& backend, const std::string& match) {
    const std::vector<WheelInfo> wheels = backend.list();
    for (const WheelInfo& info : wheels) {
        const bool matches = deviceMatches(info.name, info.vendorId, info.productId, match);
        WE_LOG_DEBUG(log(), "candidate '{}' ({:04x}:{:04x}) {}", info.name, info.vendorId, info.productId,
                     matches ? "matches" : "skipped");
        if (!matches) {
            continue;
        }
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
    WE_LOG_ERROR(log(), "no game controller name or vid:pid contains '{}' ({} connected); check [emulator] "
                        "source_device",
                 match, wheels.size());
    return nullptr;
}

bool wheelConnected(WheelBackend& backend, const std::string& match) {
    for (const WheelInfo& info : backend.list()) {
        if (deviceMatches(info.name, info.vendorId, info.productId, match)) return true;
    }
    return false;
}

}  // namespace wheelemu::service
