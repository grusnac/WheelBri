#pragma once

#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/WheelState.hpp"

#include <array>
#include <string>
#include <vector>

namespace wheelbri {

// The mapped axes (steering, the pedals) that their device does not have, one line each ("throttle: axis 6 of the
// wheel, which has 3 axes"). axisCounts[d]: how many axes device d reports (0 the wheel, 1..3 the accessories); a
// negative count is unknown and not checked. Such an axis only ever reads centred (steering) or released (a pedal).
[[nodiscard]] std::vector<std::string> missingMappedAxes(const MapperConfig& config,
                                                         const std::array<int, kMaxInputDevices>& axisCounts);

// Converts the physical wheel's raw state into the target-agnostic WheelState.
// Pure and allocation-free: safe to call on every pass of the service loop.
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

}  // namespace wheelbri
