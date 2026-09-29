#include "SdlRuntime.hpp"

#include "wheelemu/Logging.hpp"

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.sdl");
    return logger;
}

int connectedCount() {
    int count = 0;
    SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
    SDL_free(ids);
    return count;
}

// Some backends (macOS IOKit, Linux udev) report devices asynchronously after SDL_Init, so an
// immediate SDL_GetJoysticks() can be empty. Pump events until the device count stops changing.
void waitForDeviceDiscovery() {
    constexpr Uint64 kMaxWaitNs = 1500 * SDL_NS_PER_MS;
    constexpr Uint64 kStableForNs = 300 * SDL_NS_PER_MS;
    constexpr Uint32 kPollMs = 20;

    const Uint64 start = SDL_GetTicksNS();
    Uint64 lastChange = start;
    int count = connectedCount();
    while (SDL_GetTicksNS() - start < kMaxWaitNs && SDL_GetTicksNS() - lastChange < kStableForNs) {
        SDL_PumpEvents();
        SDL_UpdateJoysticks();
        const int now = connectedCount();
        if (now != count) {
            WE_LOG_DEBUG(log(), "device discovery: {} -> {} joystick(s) after {} ms", count, now,
                         (SDL_GetTicksNS() - start) / SDL_NS_PER_MS);
            count = now;
            lastChange = SDL_GetTicksNS();
        }
        SDL_Delay(kPollMs);
    }
    WE_LOG_DEBUG(log(), "device discovery settled on {} joystick(s) in {} ms", count,
                 (SDL_GetTicksNS() - start) / SDL_NS_PER_MS);
}

}  // namespace

SdlRuntime::SdlRuntime() {
    // Keep reading the wheel while the game window has focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    ok_ = SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC);
    if (ok_) {
        WE_LOG_DEBUG(log(), "SDL joystick + haptic subsystems initialised");
        waitForDeviceDiscovery();
    } else {
        WE_LOG_CRITICAL(log(), "SDL_Init(JOYSTICK | HAPTIC) failed: {}", SDL_GetError());
    }
}

SdlRuntime::~SdlRuntime() {
    if (ok_) {
        SDL_Quit();
        WE_LOG_DEBUG(log(), "SDL shut down");
    }
}

std::vector<JoystickInfo> listJoysticks() {
    std::vector<JoystickInfo> result;
    int count = 0;
    SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
    if (ids == nullptr) {
        WE_LOG_ERROR(log(), "SDL_GetJoysticks failed: {}", SDL_GetError());
        return result;
    }
    for (int i = 0; i < count; ++i) {
        const char* const name = SDL_GetJoystickNameForID(ids[i]);
        result.push_back({ids[i], name != nullptr ? name : "(unnamed)", SDL_GetJoystickVendorForID(ids[i]),
                          SDL_GetJoystickProductForID(ids[i])});
    }
    SDL_free(ids);
    WE_LOG_DEBUG(log(), "{} joystick(s) connected", result.size());
    return result;
}

}  // namespace wheelemu::service
