#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wheelemu::service {

// RAII for SDL's joystick + haptic subsystems.
class SdlRuntime {
public:
    SdlRuntime();
    ~SdlRuntime();
    SdlRuntime(const SdlRuntime&) = delete;
    SdlRuntime& operator=(const SdlRuntime&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

struct JoystickInfo {
    SDL_JoystickID id = 0;
    std::string name;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
};

[[nodiscard]] std::vector<JoystickInfo> listJoysticks();

}  // namespace wheelemu::service
