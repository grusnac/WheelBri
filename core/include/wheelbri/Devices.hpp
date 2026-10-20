#pragma once

#include "wheelbri/Ffb.hpp"
#include "wheelbri/WheelState.hpp"

#include <cstddef>
#include <string>

namespace wheelbri {

// The physical wheel.
class IInputSource {
public:
    virtual ~IInputSource() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    // Refreshes `state` with the latest device values. Returns false once the device is gone.
    virtual bool poll(RawInputState& state) = 0;
};

// Plays force feedback on the physical wheel. Slots let a game run several effects at once.
class IFfbSink {
public:
    // The game's effects: slots 0 to kMaxSlots - 1 (what the shim, the virtual wheel's driver and the game share).
    static constexpr std::size_t kMaxSlots = 8;
    // One more on the wheel, WheelBri's own: the target's centring spring (FfbController::setCentringSpring), which
    // no game can stop or overwrite. A sink takes slots 0 to kWheelSlots - 1.
    static constexpr std::size_t kCentringSpringSlot = kMaxSlots;
    static constexpr std::size_t kWheelSlots = kMaxSlots + 1;

    virtual ~IFfbSink() = default;

    virtual bool play(std::size_t slot, const FfbEffect& effect) = 0;
    virtual void stop(std::size_t slot) = 0;
    virtual void stopAll() = 0;
};

}  // namespace wheelbri
