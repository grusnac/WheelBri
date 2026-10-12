#pragma once

#include "wheelemu/MapperConfig.hpp"
#include "wheelemu/WheelState.hpp"

namespace wheelemu {

// Converts the physical wheel's raw state into the target-agnostic WheelState.
// Pure and allocation-free: safe to call from the 1 kHz loop.
class Mapper {
public:
    explicit Mapper(const MapperConfig& config);

    // Every device's state (device 0 is the wheel; sources on other devices read theirs).
    [[nodiscard]] WheelState map(const RawInputs& inputs) const;
    // The wheel only (no accessories).
    [[nodiscard]] WheelState map(const RawInputState& raw) const;

    [[nodiscard]] const MapperConfig& config() const { return config_; }

private:
    [[nodiscard]] float mapSteering(const RawInputs& inputs) const;
    [[nodiscard]] static float mapPedal(const PedalConfig& pedal, const RawInputs& inputs);
    [[nodiscard]] std::uint32_t mapButtons(const RawInputs& inputs) const;
    [[nodiscard]] std::int8_t mapHat(const RawInputs& inputs) const;

    MapperConfig config_;
};

}  // namespace wheelemu
