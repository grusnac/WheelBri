#pragma once

#include "wheelemu/MapperConfig.hpp"
#include "wheelemu/WheelState.hpp"

namespace wheelemu {

// Converts the physical wheel's raw state into the target-agnostic WheelState.
// Pure and allocation-free: safe to call from the 1 kHz loop.
class Mapper {
public:
    explicit Mapper(const MapperConfig& config);

    [[nodiscard]] WheelState map(const RawInputState& raw) const;

    [[nodiscard]] const MapperConfig& config() const { return config_; }

private:
    [[nodiscard]] float mapSteering(const RawInputState& raw) const;
    [[nodiscard]] static float mapPedal(const PedalConfig& pedal, const RawInputState& raw);
    [[nodiscard]] std::uint32_t mapButtons(std::uint64_t sourceButtons) const;
    [[nodiscard]] std::int8_t mapHat(const RawInputState& raw) const;

    MapperConfig config_;
};

}  // namespace wheelemu
