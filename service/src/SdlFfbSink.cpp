#include "SdlFfbSink.hpp"

#include "wheelemu/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ffb");
    return logger;
}

std::string describeFeatures(const Uint32 features) {
    struct Named {
        Uint32 bit;
        const char* name;
    };
    static const Named kNames[] = {
        {SDL_HAPTIC_CONSTANT, "constant"}, {SDL_HAPTIC_SINE, "sine"}, {SDL_HAPTIC_SQUARE, "square"},
        {SDL_HAPTIC_TRIANGLE, "triangle"}, {SDL_HAPTIC_SAWTOOTHUP, "sawtooth-up"},
        {SDL_HAPTIC_SAWTOOTHDOWN, "sawtooth-down"}, {SDL_HAPTIC_RAMP, "ramp"}, {SDL_HAPTIC_SPRING, "spring"},
        {SDL_HAPTIC_DAMPER, "damper"}, {SDL_HAPTIC_INERTIA, "inertia"}, {SDL_HAPTIC_FRICTION, "friction"},
        {SDL_HAPTIC_LEFTRIGHT, "left-right"}, {SDL_HAPTIC_CUSTOM, "custom"}, {SDL_HAPTIC_GAIN, "gain"},
        {SDL_HAPTIC_AUTOCENTER, "autocenter"}, {SDL_HAPTIC_STATUS, "status"}, {SDL_HAPTIC_PAUSE, "pause"},
    };
    std::string out;
    for (const Named& named : kNames) {
        if ((features & named.bit) != 0) {
            out += out.empty() ? "" : ", ";
            out += named.name;
        }
    }
    return out.empty() ? "none" : out;
}

const char* effectName(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return "constant";
        case FfbEffectType::Spring: return "spring";
        case FfbEffectType::Damper: return "damper";
        case FfbEffectType::Friction: return "friction";
        case FfbEffectType::Inertia: return "inertia";
        case FfbEffectType::Sine: return "sine";
        case FfbEffectType::Square: return "square";
        case FfbEffectType::Triangle: return "triangle";
        case FfbEffectType::SawtoothUp: return "sawtooth-up";
        case FfbEffectType::SawtoothDown: return "sawtooth-down";
    }
    return "?";
}

Sint16 toSigned(const float value) {
    return static_cast<Sint16>(std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
}

Uint16 toUnsigned(const float value) {
    return static_cast<Uint16>(std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F));
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

SDL_HapticDirection steeringDirection() {
    SDL_HapticDirection direction{};
    direction.type = SDL_HAPTIC_STEERING_AXIS;
    return direction;
}

}  // namespace

std::unique_ptr<SdlFfbSink> SdlFfbSink::open(SDL_Joystick* const joystick, const bool invert) {
    SDL_Haptic* const haptic = SDL_OpenHapticFromJoystick(joystick);
    if (haptic == nullptr) {
        WE_LOG_ERROR(log(), "cannot open force feedback on this device: {}. On Linux check that the kernel driver "
                            "exposes EV_FF (evtest) and that you can write the event node",
                     SDL_GetError());
        return nullptr;
    }
    return std::unique_ptr<SdlFfbSink>(new SdlFfbSink(haptic, invert));
}

SdlFfbSink::SdlFfbSink(SDL_Haptic* const haptic, const bool invert)
    : haptic_(haptic), invert_(invert), features_(SDL_GetHapticFeatures(haptic)) {
    const char* const name = SDL_GetHapticName(haptic_);
    WE_LOG_INFO(log(), "force feedback on '{}': {} (max {} effects, {} playing at once), invert={}",
                name != nullptr ? name : "?", describeFeatures(features_), SDL_GetMaxHapticEffects(haptic_),
                SDL_GetMaxHapticEffectsPlaying(haptic_), invert_);
    if ((features_ & (SDL_HAPTIC_CONSTANT | SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER)) == 0) {
        WE_LOG_WARN(log(), "device reports none of constant/spring/damper; racing-game force feedback will be missing");
    }
    if ((features_ & SDL_HAPTIC_GAIN) != 0 && !SDL_SetHapticGain(haptic_, 100)) {  // user gain lives in the limiter
        WE_LOG_WARN(log(), "SDL_SetHapticGain failed: {}", SDL_GetError());
    }
    if ((features_ & SDL_HAPTIC_AUTOCENTER) != 0 && !SDL_SetHapticAutocenter(haptic_, 0)) {  // game spring centres
        WE_LOG_WARN(log(), "SDL_SetHapticAutocenter failed: {}", SDL_GetError());
    }
}

SdlFfbSink::~SdlFfbSink() {
    stopAll();
    for (Slot& slot : slots_) {
        release(slot);
    }
    SDL_CloseHaptic(haptic_);
}

bool SdlFfbSink::play(const std::size_t slotIndex, const FfbEffect& effect) {
    if (slotIndex >= slots_.size()) {
        return false;
    }
    static logging::RateLimiter unsupportedLimiter(std::chrono::seconds(30));
    static logging::RateLimiter failureLimiter(std::chrono::seconds(5));

    const SDL_HapticEffectType type = sdlType(effect.type);
    if ((features_ & type) == 0) {
        WE_LOG_WARN_LIMITED(log(), unsupportedLimiter, "slot {}: device cannot play {} effects, dropped", slotIndex,
                            effectName(effect.type));
        return false;
    }

    Slot& slot = slots_[slotIndex];
    const SDL_HapticEffect data = toSdl(effect);

    if (slot.id >= 0 && slot.type == type) {
        if (!SDL_UpdateHapticEffect(haptic_, slot.id, &data)) {
            WE_LOG_ERROR_LIMITED(log(), failureLimiter, "slot {}: updating {} effect failed: {}", slotIndex,
                                 effectName(effect.type), SDL_GetError());
            return false;
        }
    } else {
        release(slot);
        slot.id = SDL_CreateHapticEffect(haptic_, &data);
        if (slot.id < 0) {
            WE_LOG_ERROR_LIMITED(log(), failureLimiter, "slot {}: creating {} effect failed: {}", slotIndex,
                                 effectName(effect.type), SDL_GetError());
            return false;
        }
        slot.type = type;
        WE_LOG_DEBUG(log(), "slot {}: created {} effect (SDL id {})", slotIndex, effectName(effect.type), slot.id);
    }

    if (!slot.running) {
        if (!SDL_RunHapticEffect(haptic_, slot.id, 1)) {
            WE_LOG_ERROR_LIMITED(log(), failureLimiter, "slot {}: starting {} effect failed: {}", slotIndex,
                                 effectName(effect.type), SDL_GetError());
            return false;
        }
        slot.running = true;
        WE_LOG_DEBUG(log(), "slot {}: {} effect started", slotIndex, effectName(effect.type));
    }
    return true;
}

void SdlFfbSink::stop(const std::size_t slotIndex) {
    if (slotIndex >= slots_.size()) {
        return;
    }
    Slot& slot = slots_[slotIndex];
    if (slot.id >= 0 && slot.running) {
        if (!SDL_StopHapticEffect(haptic_, slot.id)) {
            WE_LOG_WARN(log(), "slot {}: stopping effect failed: {}", slotIndex, SDL_GetError());
        }
        slot.running = false;
        WE_LOG_DEBUG(log(), "slot {}: stopped", slotIndex);
    }
}

void SdlFfbSink::stopAll() {
    if (!SDL_StopHapticEffects(haptic_)) {
        WE_LOG_WARN(log(), "stopping all effects failed: {}", SDL_GetError());
    }
    WE_LOG_DEBUG(log(), "all effects stopped");
    for (Slot& slot : slots_) {
        slot.running = false;
    }
}

void SdlFfbSink::release(Slot& slot) {
    if (slot.id >= 0) {
        SDL_DestroyHapticEffect(haptic_, slot.id);
    }
    slot = Slot{};
}

SDL_HapticEffect SdlFfbSink::toSdl(const FfbEffect& effect) const {
    const float sign = invert_ ? -1.0F : 1.0F;
    SDL_HapticEffect data{};
    data.type = sdlType(effect.type);

    switch (effect.type) {
        case FfbEffectType::Constant:
            data.constant.type = data.type;
            data.constant.direction = steeringDirection();
            data.constant.length = SDL_HAPTIC_INFINITY;
            data.constant.level = toSigned(sign * effect.constantLevel);
            break;

        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia: {
            const FfbCondition& c = effect.condition;
            data.condition.type = data.type;
            data.condition.direction = steeringDirection();
            data.condition.length = SDL_HAPTIC_INFINITY;
            // Inverting mirrors the effect: left and right swap, the centre flips.
            const float rightCoefficient = invert_ ? c.negativeCoefficient : c.positiveCoefficient;
            const float leftCoefficient = invert_ ? c.positiveCoefficient : c.negativeCoefficient;
            const float rightSaturation = invert_ ? c.negativeSaturation : c.positiveSaturation;
            const float leftSaturation = invert_ ? c.positiveSaturation : c.negativeSaturation;
            data.condition.right_coeff[0] = toSigned(rightCoefficient);
            data.condition.left_coeff[0] = toSigned(leftCoefficient);
            data.condition.right_sat[0] = toUnsigned(rightSaturation);
            data.condition.left_sat[0] = toUnsigned(leftSaturation);
            data.condition.deadband[0] = toUnsigned(c.deadband);
            data.condition.center[0] = toSigned(sign * c.center);
            break;
        }

        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown: {
            const FfbPeriodic& p = effect.periodic;
            data.periodic.type = data.type;
            data.periodic.direction = steeringDirection();
            data.periodic.length = SDL_HAPTIC_INFINITY;
            data.periodic.period = static_cast<Uint16>(std::clamp(std::lround(p.periodMs), 1L, 65535L));
            data.periodic.magnitude = toSigned(p.magnitude);
            data.periodic.offset = toSigned(sign * p.offset);
            float phase = std::fmod(p.phaseDeg, 360.0F);
            if (phase < 0.0F) {
                phase += 360.0F;
            }
            data.periodic.phase = static_cast<Uint16>(std::lround(phase * 100.0F));
            break;
        }
    }
    return data;
}

}  // namespace wheelemu::service
