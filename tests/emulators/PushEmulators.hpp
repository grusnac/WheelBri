#pragma once

// Emulators for testing the service loop (the "push" input) without hardware or an OS: devices that announce new
// input the way DirectInput / evdev do (or fail to), the game's force feedback datagrams, a wait over them, and a rig
// that runs the service's own loop (service/src/ServiceLoop.cpp: the very code runLoop runs) with the real Pipeline
// and the real FfbController on them, all on a virtual clock. What the tests read back: when each change of a device
// was read (its latency), what woke each pass, whether and when the loop gave up on events, what reached the wheel's
// force feedback and when, the window's copy, and how the loop ended.

#include "ServiceLoop.hpp"

#include "wheelbri/Devices.hpp"
#include "wheelbri/EventLoopControl.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/Pipeline.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::emulators {

inline constexpr std::uint64_t kUs = 1'000;
inline constexpr std::uint64_t kMs = 1'000'000;
inline constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

class VirtualClock {
public:
    [[nodiscard]] std::uint64_t now() const { return nowNs_; }
    void advanceTo(const std::uint64_t ns) {
        if (ns > nowNs_) nowNs_ = ns;
    }
    void advanceBy(const std::uint64_t ns) { nowNs_ += ns; }

private:
    std::uint64_t nowNs_ = 0;
};

// A wheel or accessory whose input changes at scheduled times. Its notification behaves like DirectInput's event
// (auto-reset: one wait takes it, however many changes set it) or evdev's readable descriptor, and can be made to
// misbehave.
class EmulatedPushDevice final : public IInputSource {
public:
    struct Behaviour {
        bool notifies = true;                  // false: DirectInput only polls it (DIDC_POLLEDDEVICE)
        std::uint32_t dropEvery = 0;           // every Nth change sets no notification (0: none dropped)
        std::uint64_t stopNotifyingAtNs = kNever;   // from then on, no notification at all
        std::uint64_t notifyDelayNs = 0;       // the notification comes this long after the change
        std::uint64_t spuriousEveryNs = 0;     // notifications without a change (the R3 at rest: ~450 a second)
        std::uint64_t disconnectAtNs = kNever; // poll() fails from then on
        bool signalsDisconnect = true;         // the notification fires when it disconnects (evdev: POLLERR)
    };

    EmulatedPushDevice(std::string name, const VirtualClock& clock, const Behaviour& behaviour)
        : name_(std::move(name)), clock_(clock), behaviour_(behaviour) {}
    EmulatedPushDevice(std::string name, const VirtualClock& clock) : EmulatedPushDevice(std::move(name), clock, {}) {}

    // Schedules a change (in time order): an axis to `value`, or button `button` pressed or released.
    void moveAxis(std::uint64_t atNs, int axis, float value);
    void setButton(std::uint64_t atNs, int button, bool pressed);
    // A change every `everyNs` from `fromNs` to `toNs` (the axis sweeps back and forth): a wheel being turned.
    void sweep(std::uint64_t fromNs, std::uint64_t toNs, std::uint64_t everyNs, int axis = 0);

    [[nodiscard]] std::string name() const override { return name_; }
    bool poll(RawInputState& state) override;

    [[nodiscard]] bool notifies() const { return behaviour_.notifies; }
    // The earliest notification at or after the last one a wait took (kNever: none to come).
    [[nodiscard]] std::uint64_t nextSignalNs() const;
    // A wait took the notification at `nowNs` (auto-reset).
    void takeSignal(std::uint64_t nowNs) { pendingFromNs_ = nowNs + 1; }

    [[nodiscard]] const std::vector<std::uint64_t>& changeTimes() const { return times_; }
    [[nodiscard]] std::uint32_t droppedNotifications() const;

private:
    [[nodiscard]] bool notified(std::size_t change) const;

    std::string name_;
    const VirtualClock& clock_;
    Behaviour behaviour_;
    std::vector<std::uint64_t> times_;
    std::vector<RawInputState> states_;
    std::uint64_t pendingFromNs_ = 0;
};

// A UDP socket the game's force feedback arrives on: FfbPackets at scheduled times (a keep-alive unless said).
class EmulatedSocket {
public:
    explicit EmulatedSocket(bool waitable = true) : waitable_(waitable) {}
    // In time order.
    void arrive(std::uint64_t atNs, const FfbPacket& packet = FfbPacket{});
    void arriveEvery(std::uint64_t fromNs, std::uint64_t toNs, std::uint64_t everyNs);
    // The game plays `level` (a constant force, -1 .. 1) in `slot` from `atNs` (stops it with playing false).
    void constantForce(std::uint64_t atNs, float level, bool playing = true, std::uint8_t slot = 0);
    [[nodiscard]] bool waitable() const { return waitable_; }
    [[nodiscard]] std::uint64_t nextArrivalNs() const {
        return next_ < arrivals_.size() ? arrivals_[next_].atNs : kNever;
    }
    // Every datagram that has arrived by `nowNs`; the latency of each is recorded.
    [[nodiscard]] std::vector<FfbPacket> readAll(std::uint64_t nowNs);
    [[nodiscard]] const std::vector<std::uint64_t>& latencies() const { return latencies_; }

private:
    struct Arrival {
        std::uint64_t atNs = 0;
        FfbPacket packet;
    };
    bool waitable_;
    std::vector<Arrival> arrivals_;
    std::size_t next_ = 0;
    std::uint32_t sequence_ = 0;
    std::vector<std::uint64_t> latencies_;
};

// The wheel's force feedback as the loop's FfbController drives it: every call, when (on the virtual clock).
class RecordingFfbSink final : public IFfbSink {
public:
    struct Call {
        enum class Kind { Play, Stop, StopAll } kind = Kind::Play;
        std::uint64_t atNs = 0;
        std::size_t slot = 0;
        FfbEffect effect;
    };
    explicit RecordingFfbSink(const VirtualClock& clock) : clock_(clock) {}
    bool play(std::size_t slot, const FfbEffect& effect) override;
    void stop(std::size_t slot) override;
    void stopAll() override;
    [[nodiscard]] const std::vector<Call>& calls() const { return calls_; }
    // The force the wheel pushes with at `atNs` from `slot` (a constant's level as played; 0 once stopped).
    [[nodiscard]] float levelAt(std::uint64_t atNs, std::size_t slot = 0) const;
    // When the wheel's force in `slot` last went to nothing (a stop, or a level of 0) after `fromNs`.
    [[nodiscard]] std::optional<std::uint64_t> stoppedAfter(std::uint64_t fromNs, std::size_t slot = 0) const;

private:
    const VirtualClock& clock_;
    std::vector<Call> calls_;
};

// platform::WaitSet on the virtual clock: waitUntil moves the clock to the first notification or datagram, or to the
// deadline. Several ready at the same time: the first added.
class EmulatedWaitSet {
public:
    explicit EmulatedWaitSet(VirtualClock& clock) : clock_(clock) {}
    int addDevice(EmulatedPushDevice& device);
    int addSocket(EmulatedSocket& socket);
    // From `atNs` on, every wait fails (kWaitFailed) at once.
    void failFrom(const std::uint64_t atNs) { failFromNs_ = atNs; }
    int waitUntil(std::uint64_t deadlineNs);
    [[nodiscard]] std::uint64_t waits() const { return waits_; }

private:
    struct Source {
        EmulatedPushDevice* device = nullptr;
        EmulatedSocket* socket = nullptr;
    };
    VirtualClock& clock_;
    std::vector<Source> sources_;
    std::uint64_t failFromNs_ = kNever;
    std::uint64_t waits_ = 0;
};

// The service's own loop (service::ServiceLoop, which runLoop runs) on emulated devices and a virtual clock, with the
// real Pipeline and, when a game's socket is added, the real FfbController on a recording sink.
class PushLoopRig {
public:
    struct Options {
        bool windowOpen = false;
        std::uint64_t passWorkNs = 50 * kUs;   // what one pass costs
        EventLoopControl::Settings settings;
        MapperConfig mapper;                   // the mapping (default: nothing mapped but the wheel's own axes)
        std::uint64_t closeWindowAtNs = kNever;   // the window side ends the loop then (LoopShared::stop)
    };

    PushLoopRig(VirtualClock& clock, EmulatedPushDevice& wheel, const Options& options);
    PushLoopRig(VirtualClock& clock, EmulatedPushDevice& wheel) : PushLoopRig(clock, wheel, Options{}) {}
    ~PushLoopRig();
    PushLoopRig(const PushLoopRig&) = delete;
    PushLoopRig& operator=(const PushLoopRig&) = delete;

    // Before run(): what else the loop waits for. A game's socket brings force feedback (one socket per rig).
    void addAccessory(EmulatedPushDevice& accessory);
    void addGameFfb(EmulatedSocket& socket);
    EmulatedWaitSet& waitSet() { return waitSet_; }

    // Runs the loop until the clock reaches `endNs`, a device is lost or the window closes; its result.
    int run(std::uint64_t endNs);

    // After run().
    [[nodiscard]] const EventLoopControl& control() const;
    [[nodiscard]] int result() const { return result_; }
    [[nodiscard]] std::uint64_t endedAtNs() const { return endedAtNs_; }
    // When the loop started polling during the run (not at setup: control().pollReason() says that).
    [[nodiscard]] std::optional<std::uint64_t> pollingFromNs() const;
    [[nodiscard]] std::optional<std::uint64_t> lostAtNs() const { return lostAtNs_; }
    [[nodiscard]] std::uint64_t passes() const { return passTimes_.size(); }
    [[nodiscard]] std::uint64_t passesBetween(std::uint64_t fromNs, std::uint64_t toNs) const;
    [[nodiscard]] std::uint64_t wokenBy(WakeReason reason) const { return wokenBy_[static_cast<std::size_t>(reason)]; }
    // For each change of `device` before `untilNs`: how long until a pass read it (kNever: no pass after it).
    [[nodiscard]] std::vector<std::uint64_t> latencies(const EmulatedPushDevice& device,
                                                       std::uint64_t untilNs = kNever) const;
    [[nodiscard]] std::uint64_t maxLatency(const EmulatedPushDevice& device, std::uint64_t fromNs = 0,
                                           std::uint64_t untilNs = kNever) const;
    // The shortest time between two passes (the flood guard keeps it at minGapNs or more).
    [[nodiscard]] std::uint64_t minPassGap() const;
    // The window's copy as the loop last handed it over.
    [[nodiscard]] const service::UiFrame& frame() const { return shared_.frame; }
    // What the game's force feedback did on the wheel (with a game's socket added).
    [[nodiscard]] const RecordingFfbSink& ffbSink() const { return sink_; }
    // When the loop sent the app's log level to the state's reader.
    [[nodiscard]] const std::vector<std::uint64_t>& logLevelTimes() const { return logLevelTimes_; }

private:
    class System;
    class GameFfb;
    class CountingPublisher;

    VirtualClock& clock_;
    EmulatedPushDevice& wheel_;
    Options options_;
    EmulatedWaitSet waitSet_;
    std::array<IInputSource*, MapperConfig::kMaxAccessories> accessories_{};
    std::size_t accessoryCount_ = 0;
    EmulatedSocket* socket_ = nullptr;
    RecordingFfbSink sink_;
    service::LoopShared shared_;
    std::unique_ptr<service::ServiceLoop> loop_;
    std::vector<std::uint64_t> passTimes_;
    std::array<std::uint64_t, static_cast<std::size_t>(WakeReason::kCount)> wokenBy_{};
    std::vector<std::uint64_t> logLevelTimes_;
    std::optional<std::uint64_t> lostAtNs_;
    int result_ = 0;
    std::uint64_t endedAtNs_ = 0;
};

}  // namespace wheelbri::emulators
