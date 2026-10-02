#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/Devices.hpp"
#include "wheelemu/Mapper.hpp"
#include "wheelemu/UdpLoopback.hpp"

#include <optional>

namespace wheelemu {

// One tick of the input path: physical wheel -> mapping -> emulated device encoding -> shim.
class Pipeline {
public:
    Pipeline(IInputSource& source, const Mapper& mapper, const IDeviceProfile& profile, IStatePublisher& publisher);

    // Returns false once the input source is gone. `clutchOverride` (0..1 pressed), when set, replaces the
    // mapped clutch: the test clutch while no clutch pedal is connected.
    bool tick(std::optional<float> clutchOverride = std::nullopt);

    [[nodiscard]] const RawInputState& lastRaw() const { return raw_; }
    [[nodiscard]] const WheelState& lastState() const { return state_; }
    [[nodiscard]] const TargetReport& lastReport() const { return packet_.report; }
    [[nodiscard]] std::uint32_t sequence() const { return packet_.sequence; }
    [[nodiscard]] std::uint64_t publishFailures() const { return publishFailures_; }

private:
    IInputSource& source_;
    const Mapper& mapper_;
    const IDeviceProfile& profile_;
    IStatePublisher& publisher_;

    RawInputState raw_;
    WheelState state_;
    StatePacket packet_;
    std::uint64_t publishFailures_ = 0;
};

}  // namespace wheelemu
