// platform::DeviceChanges (Platform.hpp): the system's notices of game controllers coming or going, counted in a core
// ChangeCounter. The notices themselves are started per system: watchDeviceChanges in PlatformWin32.cpp /
// PlatformPosix.cpp.

#include "Platform.hpp"

#include "wheelbri/Logging.hpp"

#include <functional>

namespace wheelbri::service::platform {

namespace detail {
// Starts the system's notices of game controllers coming or going; `onChange` is called (from a system thread) for
// each. False when the system refused them. Defined per system; kept for the life of the process.
bool watchDeviceChanges(std::function<void()> onChange);
}  // namespace detail

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.devices");
    return logger;
}

}  // namespace

DeviceChanges& DeviceChanges::shared() {
    static DeviceChanges* const instance = new DeviceChanges();   // never deleted: see the class
    return *instance;
}

DeviceChanges::DeviceChanges() {
    ChangeCounter* const counter = &counter_;
    available_ = detail::watchDeviceChanges([counter] { counter->announce(); });
    if (available_) {
        WE_LOG_INFO(log(), "devices: listed again when the system says a game controller came or went");
    } else {
        WE_LOG_WARN(log(), "devices: the system gives no notice of game controllers coming or going; they are listed "
                           "every 2 s instead");
    }
}

}  // namespace wheelbri::service::platform
