// SPDX-License-Identifier: GPL-3.0-or-later
// The app's DeviceWatch lister: the system's game-controller API (startWheelBackend). Apart from DeviceWatch.cpp so
// DeviceWatchTest builds without DirectInput.

#include "DeviceWatch.hpp"

#include <memory>

namespace wheelbri::service {

DeviceWatch::Lister DeviceWatch::systemLister() {
    // Started on first use, on the watch's thread; tried again on the next call when it could not start (it logs why).
    auto backend = std::make_shared<std::unique_ptr<WheelBackend>>();
    return [backend]() -> std::optional<std::vector<WheelInfo>> {
        if (*backend == nullptr) *backend = startWheelBackend();
        if (*backend == nullptr) return std::nullopt;
        return (*backend)->list();
    };
}

}  // namespace wheelbri::service
