#pragma once

#include "wheelemu/Devices.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string>

namespace wheelemu::service {

class SdlInputSource final : public IInputSource {
public:
    // Opens the first joystick whose name contains `nameMatch` (case-insensitive). Returns nullptr if none.
    [[nodiscard]] static std::unique_ptr<SdlInputSource> open(const std::string& nameMatch);
    ~SdlInputSource() override;

    SdlInputSource(const SdlInputSource&) = delete;
    SdlInputSource& operator=(const SdlInputSource&) = delete;

    [[nodiscard]] std::string name() const override { return name_; }
    bool poll(RawInputState& state) override;

    // For the force feedback sink, which must be opened from the same joystick.
    [[nodiscard]] SDL_Joystick* joystick() const { return joystick_; }

private:
    SdlInputSource(SDL_Joystick* joystick, std::string name);

    SDL_Joystick* joystick_;
    std::string name_;
};

}  // namespace wheelemu::service
