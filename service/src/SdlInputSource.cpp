#include "SdlInputSource.hpp"

#include "SdlRuntime.hpp"

#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// SDL hat bitmask -> 0..7 clockwise from up.
std::int8_t hatToIndex(const Uint8 hat) {
    switch (hat) {
        case SDL_HAT_UP: return 0;
        case SDL_HAT_RIGHTUP: return 1;
        case SDL_HAT_RIGHT: return 2;
        case SDL_HAT_RIGHTDOWN: return 3;
        case SDL_HAT_DOWN: return 4;
        case SDL_HAT_LEFTDOWN: return 5;
        case SDL_HAT_LEFT: return 6;
        case SDL_HAT_LEFTUP: return 7;
        default: return kHatCentered;
    }
}

void logOpenedDevice(SDL_Joystick* const joystick, const JoystickInfo& info) {
    const char* const path = SDL_GetJoystickPath(joystick);
    const int axes = SDL_GetNumJoystickAxes(joystick);
    WE_LOG_INFO(log(), "opened '{}' ({:04x}:{:04x}) axes={} buttons={} hats={} path={}", info.name, info.vendorId,
                info.productId, axes, SDL_GetNumJoystickButtons(joystick), SDL_GetNumJoystickHats(joystick),
                path != nullptr ? path : "?");
    if (axes > static_cast<int>(RawInputState::kMaxAxes)) {
        WE_LOG_WARN(log(), "device has {} axes, only the first {} are read", axes, RawInputState::kMaxAxes);
    }
    for (int i = 0; i < std::min(axes, static_cast<int>(RawInputState::kMaxAxes)); ++i) {
        Sint16 initial = 0;
        const bool known = SDL_GetJoystickAxisInitialState(joystick, i, &initial);
        WE_LOG_DEBUG(log(), "  axis {}: initial value {}", i, known ? std::to_string(initial) : std::string("unknown"));
    }
}

}  // namespace

std::unique_ptr<SdlInputSource> SdlInputSource::open(const std::string& nameMatch) {
    const std::string needle = toLower(nameMatch);
    const std::vector<JoystickInfo> joysticks = listJoysticks();
    for (const JoystickInfo& info : joysticks) {
        // Match the name, or the "vvvv:pppp" USB id, so "346e" (Moza's vendor id) or "346e:0004" also work.
        char ids[10];
        std::snprintf(ids, sizeof(ids), "%04x:%04x", info.vendorId, info.productId);
        const bool matches = toLower(info.name).find(needle) != std::string::npos ||
                             std::string(ids).find(needle) != std::string::npos;
        WE_LOG_DEBUG(log(), "candidate '{}' ({:04x}:{:04x}) {}", info.name, info.vendorId, info.productId,
                     matches ? "matches" : "skipped");
        if (!matches) {
            continue;
        }
        SDL_Joystick* const joystick = SDL_OpenJoystick(info.id);
        if (joystick == nullptr) {
            WE_LOG_ERROR(log(), "cannot open '{}': {}", info.name, SDL_GetError());
            continue;
        }
        logOpenedDevice(joystick, info);
        return std::unique_ptr<SdlInputSource>(new SdlInputSource(joystick, info.name));
    }
    WE_LOG_ERROR(log(), "no joystick name or vid:pid contains '{}' ({} device(s) connected); check [emulator] source_device",
                 nameMatch, joysticks.size());
    return nullptr;
}

SdlInputSource::SdlInputSource(SDL_Joystick* const joystick, std::string name)
    : joystick_(joystick), name_(std::move(name)) {}

SdlInputSource::~SdlInputSource() {
    SDL_CloseJoystick(joystick_);
    WE_LOG_DEBUG(log(), "closed '{}'", name_);
}

bool SdlInputSource::poll(RawInputState& state) {
    if (!SDL_JoystickConnected(joystick_)) {
        WE_LOG_ERROR(log(), "'{}' was disconnected: {}", name_, SDL_GetError());
        return false;
    }

    const int axes = std::min(SDL_GetNumJoystickAxes(joystick_), static_cast<int>(RawInputState::kMaxAxes));
    for (int i = 0; i < axes; ++i) {
        // Sint16 -> [-1, 1]; -32768 maps slightly past -1, so clamp.
        const float value = static_cast<float>(SDL_GetJoystickAxis(joystick_, i)) / 32767.0F;
        state.axes[static_cast<std::size_t>(i)] = std::clamp(value, -1.0F, 1.0F);
    }

    const int buttons = std::min(SDL_GetNumJoystickButtons(joystick_), 64);
    std::uint64_t pressed = 0;
    for (int i = 0; i < buttons; ++i) {
        if (SDL_GetJoystickButton(joystick_, i)) {
            pressed |= 1ULL << i;
        }
    }
    if (pressed != state.buttons) {
        WE_LOG_TRACE(log(), "buttons {:016x} -> {:016x}", state.buttons, pressed);
    }
    state.buttons = pressed;
    state.hat = SDL_GetNumJoystickHats(joystick_) > 0 ? hatToIndex(SDL_GetJoystickHat(joystick_, 0)) : kHatCentered;
    return true;
}

}  // namespace wheelemu::service
