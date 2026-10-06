#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/Devices.hpp"
#include "wheelbri/Mapper.hpp"
#include "wheelbri/Pacing.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <array>
#include <optional>

namespace wheelbri {

// One tick of the input path: physical wheel -> mapping -> emulated device encoding -> shim.
class Pipeline {
public:
    Pipeline(IInputSource& source, const Mapper& mapper, const IDeviceProfile& profile, IStatePublisher& publisher);

    // The accessories the mapping reads (device 1, 2, 3; null = not used), polled after the wheel every tick.
    void setAccessories(const std::array<IInputSource*, MapperConfig::kMaxAccessories>& accessories) {
        accessories_ = accessories;
    }

    // From now on the state is published only when it changed, at most once every `minIntervalNs`, and after
    // StateSendGate::kKeepAliveNs without a change (StateSendGate); tick() then needs the time. Without it every tick
    // publishes (the tests).
    void limitSending(const std::uint64_t minIntervalNs) { gate_.emplace(minIntervalNs); }

    // Returns false once the wheel or an accessory is gone (lostDevice() says which). `clutchOverride` (0..1 pressed), when set, replaces the
    // mapped clutch: the test clutch while no clutch pedal is connected. `nowNs`: monotonic time, for limitSending.
    bool tick(std::optional<float> clutchOverride = std::nullopt, std::uint64_t nowNs = 0);

    [[nodiscard]] const RawInputState& lastRaw() const { return inputs_.devices[0]; }   // the wheel's
    [[nodiscard]] const RawInputs& lastInputs() const { return inputs_; }
    // The device that stopped delivering (0 = the wheel), after tick() returned false.
    [[nodiscard]] int lostDevice() const { return lostDevice_; }
    [[nodiscard]] const WheelState& lastState() const { return state_; }
    [[nodiscard]] const TargetReport& lastReport() const { return packet_.report; }
    [[nodiscard]] std::uint32_t sequence() const { return packet_.sequence; }   // packets published
    [[nodiscard]] const StateSendGate* sendGate() const { return gate_ ? &*gate_ : nullptr; }
    [[nodiscard]] std::uint64_t publishFailures() const { return publishFailures_; }

private:
    IInputSource& source_;
    const Mapper& mapper_;
    const IDeviceProfile& profile_;
    IStatePublisher& publisher_;

    std::array<IInputSource*, MapperConfig::kMaxAccessories> accessories_{};
    RawInputs inputs_;
    int lostDevice_ = -1;
    WheelState state_;
    StatePacket packet_;
    std::uint64_t publishFailures_ = 0;
    std::optional<StateSendGate> gate_;
};

}  // namespace wheelbri
