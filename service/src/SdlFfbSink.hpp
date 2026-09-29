#pragma once

#include "wheelemu/Devices.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <memory>

namespace wheelemu::service {

// Plays FfbEffects through SDL haptics: DirectInput force feedback on Windows, evdev EV_FF on Linux.
// Effects should already have passed the FfbSafetyLimiter.
class SdlFfbSink final : public IFfbSink {
public:
    // Returns nullptr if the joystick has no usable force feedback.
    [[nodiscard]] static std::unique_ptr<SdlFfbSink> open(SDL_Joystick* joystick, bool invert);
    ~SdlFfbSink() override;

    SdlFfbSink(const SdlFfbSink&) = delete;
    SdlFfbSink& operator=(const SdlFfbSink&) = delete;

    bool play(std::size_t slot, const FfbEffect& effect) override;
    void stop(std::size_t slot) override;
    void stopAll() override;

    [[nodiscard]] Uint32 features() const { return features_; }

private:
    struct Slot {
        SDL_HapticEffectID id = -1;
        SDL_HapticEffectType type = 0;
        bool running = false;
    };

    SdlFfbSink(SDL_Haptic* haptic, bool invert);

    [[nodiscard]] SDL_HapticEffect toSdl(const FfbEffect& effect) const;
    void release(Slot& slot);

    SDL_Haptic* haptic_;
    bool invert_;
    Uint32 features_;
    std::array<Slot, kMaxSlots> slots_{};
};

}  // namespace wheelemu::service
