#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/Devices.hpp"
#include "wheelbri/Mapper.hpp"
#include "wheelbri/Pacing.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <array>
#include <atomic>
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

    // Which pedal each published pedal field carries: a game profile's (GameProfiles.hpp, routePedals) while that game
    // runs with the virtual G920, else straight through. Safe from another thread; the next tick uses it.
    // lastReport() stays as mapped (the user's pedals).
    void setPedalRoute(const PedalRoute route) {
        const std::uint32_t packed = static_cast<std::uint32_t>(route.from[0]) |
                                     (static_cast<std::uint32_t>(route.from[1]) << 8U) |
                                     (static_cast<std::uint32_t>(route.from[2]) << 16U);
        pedalRoute_.store(packed, std::memory_order_relaxed);
    }
    [[nodiscard]] PedalRoute pedalRoute() const {
        const std::uint32_t packed = pedalRoute_.load(std::memory_order_relaxed);
        return PedalRoute{{static_cast<std::uint8_t>(packed & 0xFFU), static_cast<std::uint8_t>((packed >> 8U) & 0xFFU),
                           static_cast<std::uint8_t>((packed >> 16U) & 0xFFU)}};
    }


    // The order the target's buttons reach the game in ([buttons] order, ButtonOrder): Native publishes them through
    // IDeviceProfile::nativeButtons. lastReport() keeps them as named (what the window and telemetry show). Safe from
    // another thread; the next tick uses it.
    void setButtonOrder(const ButtonOrder order) {
        nativeButtons_.store(order == ButtonOrder::Native && profile_.hasNativeButtonOrder(),
                             std::memory_order_relaxed);
    }
    [[nodiscard]] bool nativeButtons() const { return nativeButtons_.load(std::memory_order_relaxed); }
    // The mapping when no game with its own values runs: the constructor's until a new one is set (the Mapping tab's
    // values changed while the session runs, LiveMapping.hpp). Safe from another thread; the next tick uses it. The
    // mapper must stay alive until the pipeline no longer ticks (the service keeps every one it made for the
    // session). Never null.
    void setMapper(const Mapper& mapper) { mapper_.store(&mapper, std::memory_order_release); }
    [[nodiscard]] const Mapper& mapper() const { return *mapper_.load(std::memory_order_acquire); }
    // The mapping while a game with its own values runs (GameTuning.hpp, tunedMapper): used instead of the one given
    // to the constructor until set back to null. Safe from another thread; the next tick uses it. The mapper must stay
    // alive until the pipeline no longer ticks (the service keeps every one it made for the whole session).
    void setGameMapper(const Mapper* mapper) { gameMapper_.store(mapper, std::memory_order_release); }
    [[nodiscard]] const Mapper* gameMapper() const { return gameMapper_.load(std::memory_order_acquire); }

    // Returns false once the wheel or an accessory is gone (lostDevice() says which). `nowNs`: monotonic time, for
    // limitSending.
    bool tick(std::uint64_t nowNs = 0);

    [[nodiscard]] const RawInputState& lastRaw() const { return inputs_.devices[0]; }   // the wheel's
    [[nodiscard]] const RawInputs& lastInputs() const { return inputs_; }
    // Whether the last tick read anything different from the tick before (any device, any axis, button or hat).
    [[nodiscard]] bool inputChanged() const { return inputChanged_; }
    // The device that stopped delivering (0 = the wheel), after tick() returned false.
    [[nodiscard]] int lostDevice() const { return lostDevice_; }
    [[nodiscard]] const WheelState& lastState() const { return state_; }
    // The mapped target report, before the game's pedal layout moved its pedals (what the window and telemetry show).
    [[nodiscard]] const TargetReport& lastReport() const { return report_; }
    [[nodiscard]] std::uint32_t sequence() const { return packet_.sequence; }   // packets published
    [[nodiscard]] const StateSendGate* sendGate() const { return gate_ ? &*gate_ : nullptr; }
    [[nodiscard]] std::uint64_t publishFailures() const { return publishFailures_; }

private:
    IInputSource& source_;
    std::atomic<const Mapper*> mapper_;
    const IDeviceProfile& profile_;
    IStatePublisher& publisher_;

    std::array<IInputSource*, MapperConfig::kMaxAccessories> accessories_{};
    RawInputs inputs_;
    int lostDevice_ = -1;
    bool inputChanged_ = false;
    WheelState state_;
    TargetReport report_;
    StatePacket packet_;
    std::atomic<const Mapper*> gameMapper_{nullptr};
    std::atomic<bool> nativeButtons_{false};
    std::atomic<std::uint32_t> pedalRoute_{0x00020100U};   // PedalRoute packed: straight through
    std::uint64_t publishFailures_ = 0;
    std::optional<StateSendGate> gate_;
};

}  // namespace wheelbri
