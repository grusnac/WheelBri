#pragma once

// Emulators for testing the service loop (the "push" input) without hardware or an OS: devices that announce new
// input the way DirectInput / evdev do (or fail to), the game's force feedback datagrams, a wait over them, and a rig
// that runs the service loop's real decisions (core EventLoopControl) and the real Pipeline on them, all on a virtual
// clock. What the tests read back: when each change of a device was read (its latency), what woke each pass, and
// whether and when the loop gave up on events.

#include "wheelbri/Devices.hpp"
#include "wheelbri/EventLoopControl.hpp"
#include "wheelbri/Pipeline.hpp"

#include <array>
#include <cstdint>
#include <limits>
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

// A UDP socket the game's force feedback arrives on.
class EmulatedSocket {
public:
    explicit EmulatedSocket(bool waitable = true) : waitable_(waitable) {}
    void arrive(std::uint64_t atNs) { arrivals_.push_back(atNs); }
    void arriveEvery(std::uint64_t fromNs, std::uint64_t toNs, std::uint64_t everyNs);
    [[nodiscard]] bool waitable() const { return waitable_; }
    [[nodiscard]] std::uint64_t nextArrivalNs() const { return next_ < arrivals_.size() ? arrivals_[next_] : kNever; }
    // Reads every datagram that has arrived by `nowNs`; the latency of each is recorded.
    void readAll(std::uint64_t nowNs);
    [[nodiscard]] const std::vector<std::uint64_t>& latencies() const { return latencies_; }

private:
    bool waitable_;
    std::vector<std::uint64_t> arrivals_;
    std::size_t next_ = 0;
    std::vector<std::uint64_t> latencies_;
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

// The service's loop (service/src/main.cpp, runLoop) on emulated devices: the same setup decisions, the same
// EventLoopControl calls around each pass and wait (a sleep instead of the wait once it failed), and the real
// Pipeline.
class PushLoopRig {
public:
    struct Options {
        bool windowOpen = false;
        bool forcesPlaying = false;
        std::uint64_t passWorkNs = 50 * kUs;   // what one pass costs
        EventLoopControl::Settings settings;
    };

    PushLoopRig(VirtualClock& clock, EmulatedPushDevice& wheel, const Options& options);
    PushLoopRig(VirtualClock& clock, EmulatedPushDevice& wheel) : PushLoopRig(clock, wheel, Options{}) {}
    ~PushLoopRig();
    PushLoopRig(const PushLoopRig&) = delete;
    PushLoopRig& operator=(const PushLoopRig&) = delete;

    // Before run(): what else the loop waits for.
    void addAccessory(EmulatedPushDevice& accessory);
    void addGameFfb(EmulatedSocket& socket);
    EmulatedWaitSet& waitSet() { return waitSet_; }

    // Runs passes until the clock reaches `endNs` or a device is lost.
    void run(std::uint64_t endNs);

    [[nodiscard]] const EventLoopControl& control() const { return control_; }
    // When the loop started polling during the run (not at setup: control().pollReason() says that).
    [[nodiscard]] std::optional<std::uint64_t> pollingFromNs() const { return pollingFromNs_; }
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

private:
    class CountingPublisher;

    VirtualClock& clock_;
    EmulatedPushDevice& wheel_;
    Options options_;
    EventLoopControl control_;
    EmulatedWaitSet waitSet_;
    std::vector<WakeReason> reasons_;
    std::array<IInputSource*, MapperConfig::kMaxAccessories> accessories_{};
    std::size_t accessoryCount_ = 0;
    std::vector<EmulatedSocket*> sockets_;
    std::vector<std::uint64_t> passTimes_;
    std::array<std::uint64_t, static_cast<std::size_t>(WakeReason::kCount)> wokenBy_{};
    std::optional<std::uint64_t> pollingFromNs_;
    std::optional<std::uint64_t> lostAtNs_;
};

}  // namespace wheelbri::emulators
