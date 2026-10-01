// The physical wheel through SDL3: the backend for systems without a native one (macOS), and a fallback on
// Windows and Linux when built with -DWE_USE_SDL=ON. Reading and force feedback as the service did before
// it had its own DirectInput and evdev code.

#include "Wheel.hpp"

#ifdef _WIN32
#include "DinputReader.hpp"
#endif

#include "wheelemu/Logging.hpp"
#include "wheelemu/WheelOutput.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdlib>

namespace wheelemu::service {

namespace {

const logging::Logger& sdlLog() {
    static const logging::Logger logger = logging::get("service.sdl");
    return logger;
}

const logging::Logger& inputLog() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

const logging::Logger& ffbLog() {
    static const logging::Logger logger = logging::get("service.ffb");
    return logger;
}

// SDL's own log lines into the service's log (warnings and errors; SDL_LOGGING=... for more).
spdlog::level::level_enum toSpdlogLevel(const SDL_LogPriority priority) {
    switch (priority) {
        case SDL_LOG_PRIORITY_TRACE:
        case SDL_LOG_PRIORITY_VERBOSE: return spdlog::level::trace;
        case SDL_LOG_PRIORITY_DEBUG: return spdlog::level::debug;
        case SDL_LOG_PRIORITY_INFO: return spdlog::level::info;
        case SDL_LOG_PRIORITY_WARN: return spdlog::level::warn;
        case SDL_LOG_PRIORITY_ERROR: return spdlog::level::err;
        case SDL_LOG_PRIORITY_CRITICAL: return spdlog::level::critical;
        default: return spdlog::level::info;
    }
}

void SDLCALL forwardSdlLog(void* /*userdata*/, const int category, const SDL_LogPriority priority,
                           const char* message) {
    sdlLog()->log(toSpdlogLevel(priority), "SDL[{}]: {}", category, message != nullptr ? message : "");
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
    const Uint64 start = SDL_GetTicksNS();
    Uint64 lastChange = start;
    int count = connectedCount();
    while (SDL_GetTicksNS() - start < kMaxWaitNs && SDL_GetTicksNS() - lastChange < kStableForNs) {
        SDL_PumpEvents();
        SDL_UpdateJoysticks();
        const int now = connectedCount();
        if (now != count) {
            count = now;
            lastChange = SDL_GetTicksNS();
        }
        SDL_Delay(20);
    }
    WE_LOG_DEBUG(sdlLog(), "device discovery settled on {} joystick(s) in {} ms", count,
                 (SDL_GetTicksNS() - start) / SDL_NS_PER_MS);
}

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

SDL_HapticEffectType sdlType(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return SDL_HAPTIC_CONSTANT;
        case FfbEffectType::Spring: return SDL_HAPTIC_SPRING;
        case FfbEffectType::Damper: return SDL_HAPTIC_DAMPER;
        case FfbEffectType::Friction: return SDL_HAPTIC_FRICTION;
        case FfbEffectType::Inertia: return SDL_HAPTIC_INERTIA;
        case FfbEffectType::Sine: return SDL_HAPTIC_SINE;
        case FfbEffectType::Square: return SDL_HAPTIC_SQUARE;
        case FfbEffectType::Triangle: return SDL_HAPTIC_TRIANGLE;
        case FfbEffectType::SawtoothUp: return SDL_HAPTIC_SAWTOOTHUP;
        case FfbEffectType::SawtoothDown: return SDL_HAPTIC_SAWTOOTHDOWN;
    }
    return 0;
}

SDL_HapticEffect toSdl(const WheelEffect& w) {
    SDL_HapticEffect data{};
    data.type = sdlType(w.type);
    SDL_HapticDirection direction{};
    direction.type = SDL_HAPTIC_STEERING_AXIS;
    switch (w.type) {
        case FfbEffectType::Constant:
            data.constant.type = data.type;
            data.constant.direction = direction;
            data.constant.length = SDL_HAPTIC_INFINITY;
            data.constant.level = w.level;
            break;
        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia:
            data.condition.type = data.type;
            data.condition.direction = direction;
            data.condition.length = SDL_HAPTIC_INFINITY;
            data.condition.right_coeff[0] = w.rightCoefficient;
            data.condition.left_coeff[0] = w.leftCoefficient;
            data.condition.right_sat[0] = w.rightSaturation;
            data.condition.left_sat[0] = w.leftSaturation;
            data.condition.deadband[0] = w.deadband;
            data.condition.center[0] = w.center;
            break;
        default:
            data.periodic.type = data.type;
            data.periodic.direction = direction;
            data.periodic.length = SDL_HAPTIC_INFINITY;
            data.periodic.period = w.periodMs;
            data.periodic.magnitude = w.magnitude;
            data.periodic.offset = w.offset;
            data.periodic.phase = w.phase;
            break;
    }
    return data;
}

class SdlFfbSink final : public IFfbSink {
public:
    SdlFfbSink(SDL_Haptic* const haptic, const bool invert)
        : haptic_(haptic), invert_(invert), features_(SDL_GetHapticFeatures(haptic)) {
        const char* const name = SDL_GetHapticName(haptic_);
        WE_LOG_INFO(ffbLog(), "force feedback on '{}' (SDL): features 0x{:08x} (max {} effects, {} at once), invert={}",
                    name != nullptr ? name : "?", features_, SDL_GetMaxHapticEffects(haptic_),
                    SDL_GetMaxHapticEffectsPlaying(haptic_), invert_);
        if ((features_ & (SDL_HAPTIC_CONSTANT | SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER)) == 0) {
            WE_LOG_WARN(ffbLog(), "device reports none of constant/spring/damper; racing-game force feedback will be "
                                  "missing");
        }
        if ((features_ & SDL_HAPTIC_GAIN) != 0 && !SDL_SetHapticGain(haptic_, 100)) {
            WE_LOG_WARN(ffbLog(), "SDL_SetHapticGain failed: {}", SDL_GetError());
        }
        if ((features_ & SDL_HAPTIC_AUTOCENTER) != 0 && !SDL_SetHapticAutocenter(haptic_, 0)) {
            WE_LOG_WARN(ffbLog(), "SDL_SetHapticAutocenter failed: {}", SDL_GetError());
        }
    }

    ~SdlFfbSink() override {
        stopAll();
        for (Slot& slot : slots_) {
            release(slot);
        }
        SDL_CloseHaptic(haptic_);
    }

    SdlFfbSink(const SdlFfbSink&) = delete;
    SdlFfbSink& operator=(const SdlFfbSink&) = delete;

    bool play(const std::size_t slotIndex, const FfbEffect& effect) override {
        if (slotIndex >= slots_.size()) {
            return false;
        }
        static logging::RateLimiter unsupportedLimiter(std::chrono::seconds(30));
        static logging::RateLimiter failureLimiter(std::chrono::seconds(5));
        const SDL_HapticEffectType type = sdlType(effect.type);
        if ((features_ & type) == 0) {
            WE_LOG_WARN_LIMITED(ffbLog(), unsupportedLimiter, "slot {}: device cannot play {} effects, dropped",
                                slotIndex, ffbTypeName(effect.type));
            return false;
        }
        Slot& slot = slots_[slotIndex];
        const SDL_HapticEffect data = toSdl(toWheelEffect(effect, invert_));
        if (slot.id >= 0 && slot.type == type) {
            if (!SDL_UpdateHapticEffect(haptic_, slot.id, &data)) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: updating {} effect failed: {}", slotIndex,
                                     ffbTypeName(effect.type), SDL_GetError());
                return false;
            }
        } else {
            release(slot);
            slot.id = SDL_CreateHapticEffect(haptic_, &data);
            if (slot.id < 0) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: creating {} effect failed: {}", slotIndex,
                                     ffbTypeName(effect.type), SDL_GetError());
                return false;
            }
            slot.type = type;
        }
        if (!slot.running) {
            if (!SDL_RunHapticEffect(haptic_, slot.id, 1)) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: starting {} effect failed: {}", slotIndex,
                                     ffbTypeName(effect.type), SDL_GetError());
                return false;
            }
            slot.running = true;
        }
        return true;
    }

    void stop(const std::size_t slotIndex) override {
        if (slotIndex >= slots_.size()) {
            return;
        }
        Slot& slot = slots_[slotIndex];
        if (slot.id >= 0 && slot.running) {
            if (!SDL_StopHapticEffect(haptic_, slot.id)) {
                WE_LOG_WARN(ffbLog(), "slot {}: stopping effect failed: {}", slotIndex, SDL_GetError());
            }
            slot.running = false;
        }
    }

    void stopAll() override {
        if (!SDL_StopHapticEffects(haptic_)) {
            WE_LOG_WARN(ffbLog(), "stopping all effects failed: {}", SDL_GetError());
        }
        for (Slot& slot : slots_) {
            slot.running = false;
        }
    }

private:
    struct Slot {
        SDL_HapticEffectID id = -1;
        SDL_HapticEffectType type = 0;
        bool running = false;
    };

    void release(Slot& slot) {
        if (slot.id >= 0) {
            SDL_DestroyHapticEffect(haptic_, slot.id);
        }
        slot = Slot{};
    }

    SDL_Haptic* haptic_;
    bool invert_;
    Uint32 features_;
    std::array<Slot, kMaxSlots> slots_{};
};

class SdlWheel final : public Wheel {
public:
    SdlWheel(WheelInfo info, SDL_Joystick* const joystick) : info_(std::move(info)), joystick_(joystick) {
#ifdef _WIN32
        // Windows: read the wheel through DirectInput directly when it numbers the inputs as SDL does, so
        // SDL's joystick update (and its device re-scans) never runs in the loop.
        std::string error;
        if (const char* const path = SDL_GetJoystickPath(joystick_)) {
            auto reader = DinputReader::open(path, error);
            if (reader != nullptr && reader->axes() == SDL_GetNumJoystickAxes(joystick_) &&
                reader->buttons() == SDL_GetNumJoystickButtons(joystick_) &&
                reader->hats() == SDL_GetNumJoystickHats(joystick_)) {
                dinput_ = std::move(reader);
            }
        }
#endif
    }

    ~SdlWheel() override { SDL_CloseJoystick(joystick_); }

    SdlWheel(const SdlWheel&) = delete;
    SdlWheel& operator=(const SdlWheel&) = delete;

    [[nodiscard]] const WheelInfo& info() const override { return info_; }
    [[nodiscard]] int axes() const override { return SDL_GetNumJoystickAxes(joystick_); }
    [[nodiscard]] int buttons() const override { return SDL_GetNumJoystickButtons(joystick_); }
    [[nodiscard]] int hats() const override { return SDL_GetNumJoystickHats(joystick_); }

    void update() override {
#ifdef _WIN32
        if (dinput_ != nullptr) return;
#endif
        SDL_UpdateJoysticks();
    }

    bool poll(RawInputState& state) override {
#ifdef _WIN32
        if (dinput_ != nullptr) {
            if (!dinput_->read(state)) {
                WE_LOG_ERROR(inputLog(), "'{}' was disconnected (DirectInput cannot read it any more)", info_.name);
                return false;
            }
            return true;
        }
#endif
        if (!SDL_JoystickConnected(joystick_)) {
            WE_LOG_ERROR(inputLog(), "'{}' was disconnected: {}", info_.name, SDL_GetError());
            return false;
        }
        const int axisCount = std::min(SDL_GetNumJoystickAxes(joystick_), static_cast<int>(RawInputState::kMaxAxes));
        for (int i = 0; i < axisCount; ++i) {
            const float value = static_cast<float>(SDL_GetJoystickAxis(joystick_, i)) / 32767.0F;
            state.axes[static_cast<std::size_t>(i)] = std::clamp(value, -1.0F, 1.0F);
        }
        const int buttonCount = std::min(SDL_GetNumJoystickButtons(joystick_), 64);
        std::uint64_t pressed = 0;
        for (int i = 0; i < buttonCount; ++i) {
            if (SDL_GetJoystickButton(joystick_, i)) {
                pressed |= 1ULL << i;
            }
        }
        state.buttons = pressed;
        state.hat = SDL_GetNumJoystickHats(joystick_) > 0 ? hatToIndex(SDL_GetJoystickHat(joystick_, 0)) : kHatCentered;
        return true;
    }

    std::unique_ptr<IFfbSink> openForceFeedback(const bool invert) override {
        SDL_Haptic* const haptic = SDL_OpenHapticFromJoystick(joystick_);
        if (haptic == nullptr) {
            WE_LOG_ERROR(ffbLog(), "cannot open force feedback on this device: {}", SDL_GetError());
            return nullptr;
        }
        return std::make_unique<SdlFfbSink>(haptic, invert);
    }

private:
    WheelInfo info_;
    SDL_Joystick* joystick_;
#ifdef _WIN32
    std::unique_ptr<DinputReader> dinput_;
#endif
};

class SdlBackend final : public WheelBackend {
public:
    ~SdlBackend() override { SDL_Quit(); }

    std::vector<WheelInfo> list() override {
        std::vector<WheelInfo> result;
        int count = 0;
        SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
        if (ids == nullptr) {
            WE_LOG_ERROR(sdlLog(), "SDL_GetJoysticks failed: {}", SDL_GetError());
            return result;
        }
        for (int i = 0; i < count; ++i) {
            const char* const name = SDL_GetJoystickNameForID(ids[i]);
            const char* const path = SDL_GetJoystickPathForID(ids[i]);
            result.push_back({name != nullptr ? name : "(unnamed)", SDL_GetJoystickVendorForID(ids[i]),
                              SDL_GetJoystickProductForID(ids[i]), path != nullptr ? path : ""});
        }
        SDL_free(ids);
        return result;
    }

    std::unique_ptr<Wheel> open(const WheelInfo& info) override {
        int count = 0;
        SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
        SDL_JoystickID found = 0;
        for (int i = 0; i < count && found == 0; ++i) {
            const char* const path = SDL_GetJoystickPathForID(ids[i]);
            const char* const name = SDL_GetJoystickNameForID(ids[i]);
            if ((path != nullptr && info.path == path) || (info.path.empty() && name != nullptr && info.name == name)) {
                found = ids[i];
            }
        }
        SDL_free(ids);
        SDL_Joystick* const joystick = found != 0 ? SDL_OpenJoystick(found) : nullptr;
        if (joystick == nullptr) {
            WE_LOG_ERROR(inputLog(), "cannot open '{}': {}", info.name, SDL_GetError());
            return nullptr;
        }
        return std::make_unique<SdlWheel>(info, joystick);
    }

    [[nodiscard]] std::string describe() const override {
        const int version = SDL_GetVersion();
        return "SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." +
               std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." + std::to_string(SDL_VERSIONNUM_MICRO(version));
    }
};

}  // namespace

std::unique_ptr<WheelBackend> startWheelBackend() {
    SDL_SetLogOutputFunction(forwardSdlLog, nullptr);
    if (std::getenv("SDL_LOGGING") == nullptr) {
        SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef _WIN32
    // Back-ends a wheel does not need: fewer devices opened, no device re-scans in the loop.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
#endif
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
        WE_LOG_CRITICAL(sdlLog(), "SDL_Init(JOYSTICK | HAPTIC) failed: {}", SDL_GetError());
        return nullptr;
    }
    waitForDeviceDiscovery();
    return std::make_unique<SdlBackend>();
}

}  // namespace wheelemu::service
