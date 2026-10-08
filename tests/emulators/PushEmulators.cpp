#include "PushEmulators.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/Mapper.hpp"

#include <algorithm>
#include <stdexcept>

namespace wheelbri::emulators {

// --- EmulatedPushDevice ---

void EmulatedPushDevice::moveAxis(const std::uint64_t atNs, const int axis, const float value) {
    if (!times_.empty() && atNs < times_.back()) throw std::invalid_argument("changes must be scheduled in time order");
    RawInputState next = states_.empty() ? RawInputState{} : states_.back();
    next.axes[static_cast<std::size_t>(axis)] = value;
    times_.push_back(atNs);
    states_.push_back(next);
}

void EmulatedPushDevice::setButton(const std::uint64_t atNs, const int button, const bool pressed) {
    if (!times_.empty() && atNs < times_.back()) throw std::invalid_argument("changes must be scheduled in time order");
    RawInputState next = states_.empty() ? RawInputState{} : states_.back();
    const std::uint64_t bit = std::uint64_t{1} << button;
    next.buttons = pressed ? (next.buttons | bit) : (next.buttons & ~bit);
    times_.push_back(atNs);
    states_.push_back(next);
}

void EmulatedPushDevice::sweep(const std::uint64_t fromNs, const std::uint64_t toNs, const std::uint64_t everyNs,
                               const int axis) {
    int step = 0;
    for (std::uint64_t t = fromNs; t < toNs; t += everyNs, ++step) {
        // -1 .. 1 and back in 200 steps; every step a different value from the one before.
        const int phase = step % 400;
        const float value = static_cast<float>(phase < 200 ? phase : 400 - phase) / 100.0F - 1.0F;
        moveAxis(t, axis, value);
    }
}

bool EmulatedPushDevice::poll(RawInputState& state) {
    const std::uint64_t now = clock_.now();
    if (now >= behaviour_.disconnectAtNs) return false;
    const auto after = std::upper_bound(times_.begin(), times_.end(), now);
    state = after == times_.begin() ? RawInputState{} : states_[static_cast<std::size_t>(after - times_.begin()) - 1];
    return true;
}

bool EmulatedPushDevice::notified(const std::size_t change) const {
    if (!behaviour_.notifies || times_[change] >= behaviour_.stopNotifyingAtNs) return false;
    return behaviour_.dropEvery == 0 || (change + 1) % behaviour_.dropEvery != 0;
}

std::uint32_t EmulatedPushDevice::droppedNotifications() const {
    std::uint32_t dropped = 0;
    for (std::size_t i = 0; i < times_.size(); ++i) dropped += notified(i) ? 0U : 1U;
    return dropped;
}

std::uint64_t EmulatedPushDevice::nextSignalNs() const {
    if (!behaviour_.notifies) return kNever;
    std::uint64_t next = kNever;
    // The first change whose notification is due at or after pendingFromNs_.
    const std::uint64_t from = pendingFromNs_ > behaviour_.notifyDelayNs ? pendingFromNs_ - behaviour_.notifyDelayNs : 0;
    for (auto it = std::lower_bound(times_.begin(), times_.end(), from); it != times_.end(); ++it) {
        if (*it >= behaviour_.stopNotifyingAtNs) break;
        if (notified(static_cast<std::size_t>(it - times_.begin()))) {
            next = *it + behaviour_.notifyDelayNs;
            break;
        }
    }
    if (behaviour_.spuriousEveryNs > 0 && pendingFromNs_ < behaviour_.stopNotifyingAtNs) {
        const std::uint64_t every = behaviour_.spuriousEveryNs;
        const std::uint64_t k = std::max<std::uint64_t>(1, (pendingFromNs_ + every - 1) / every);
        if (k * every < behaviour_.stopNotifyingAtNs) next = std::min(next, k * every);
    }
    if (behaviour_.signalsDisconnect && behaviour_.disconnectAtNs != kNever &&
        behaviour_.disconnectAtNs >= pendingFromNs_) {
        next = std::min(next, behaviour_.disconnectAtNs);
    }
    return next;
}

// --- EmulatedSocket ---

void EmulatedSocket::arriveEvery(const std::uint64_t fromNs, const std::uint64_t toNs, const std::uint64_t everyNs) {
    for (std::uint64_t t = fromNs; t < toNs; t += everyNs) arrive(t);
}

void EmulatedSocket::readAll(const std::uint64_t nowNs) {
    while (next_ < arrivals_.size() && arrivals_[next_] <= nowNs) {
        latencies_.push_back(nowNs - arrivals_[next_]);
        ++next_;
    }
}

// --- EmulatedWaitSet ---

int EmulatedWaitSet::addDevice(EmulatedPushDevice& device) {
    if (!device.notifies()) return -1;
    sources_.push_back({&device, nullptr});
    return static_cast<int>(sources_.size()) - 1;
}

int EmulatedWaitSet::addSocket(EmulatedSocket& socket) {
    if (!socket.waitable()) return -1;
    sources_.push_back({nullptr, &socket});
    return static_cast<int>(sources_.size()) - 1;
}

int EmulatedWaitSet::waitUntil(const std::uint64_t deadlineNs) {
    ++waits_;
    const std::uint64_t now = clock_.now();
    if (now >= failFromNs_) return kWaitFailed;
    std::uint64_t first = kNever;
    for (const Source& source : sources_) {
        const std::uint64_t ready = source.device != nullptr ? source.device->nextSignalNs()
                                                             : source.socket->nextArrivalNs();
        first = std::min(first, std::max(ready, now));
    }
    if (failFromNs_ <= std::min(first, deadlineNs)) {
        clock_.advanceTo(failFromNs_);
        return kWaitFailed;
    }
    if (first == kNever || first > deadlineNs) {
        clock_.advanceTo(deadlineNs);
        return kWaitTimedOut;
    }
    clock_.advanceTo(first);
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        const Source& source = sources_[i];
        const std::uint64_t ready = source.device != nullptr ? source.device->nextSignalNs()
                                                             : source.socket->nextArrivalNs();
        if (ready <= first) {
            if (source.device != nullptr) source.device->takeSignal(clock_.now());
            return static_cast<int>(i);
        }
    }
    return kWaitTimedOut;   // not reached
}

// --- PushLoopRig ---

class PushLoopRig::CountingPublisher final : public IStatePublisher {
public:
    bool publish(const StatePacket& /*packet*/) override {
        ++published;
        return true;
    }
    std::uint64_t published = 0;
};

PushLoopRig::PushLoopRig(VirtualClock& clock, EmulatedPushDevice& wheel, const Options& options)
    : clock_(clock), wheel_(wheel), options_(options), control_(options.settings),
      waitSet_(clock) {}

PushLoopRig::~PushLoopRig() = default;

void PushLoopRig::addAccessory(EmulatedPushDevice& accessory) {
    if (accessoryCount_ >= accessories_.size()) throw std::invalid_argument("at most three accessories");
    accessories_[accessoryCount_++] = &accessory;
}

void PushLoopRig::addGameFfb(EmulatedSocket& socket) { sockets_.push_back(&socket); }

void PushLoopRig::run(const std::uint64_t endNs) {
    if (!passTimes_.empty()) throw std::logic_error("run() once per rig");
    const Mapper mapper(MapperConfig{});
    const DeviceProfile profile(g920Spec());
    CountingPublisher publisher;
    Pipeline pipeline(wheel_, mapper, profile, publisher);
    pipeline.setAccessories(accessories_);

    // As runLoop: the loop waits for every device that notifies and every socket it can take; anything else makes it
    // poll as well.
    const auto addDevice = [&](EmulatedPushDevice& device) {
        if (waitSet_.addDevice(device) < 0) {
            control_.pollBecause(PollReason::NoNotification);
            return;
        }
        reasons_.push_back(WakeReason::Wheel);
    };
    addDevice(wheel_);
    for (std::size_t i = 0; i < accessoryCount_; ++i) addDevice(*static_cast<EmulatedPushDevice*>(accessories_[i]));
    for (EmulatedSocket* socket : sockets_) {
        if (waitSet_.addSocket(*socket) < 0) {
            control_.pollBecause(PollReason::SocketNotWaitable);
            continue;
        }
        reasons_.push_back(WakeReason::GameFfb);
    }

    while (clock_.now() < endNs) {
        const std::uint64_t start = clock_.now();
        if (!pipeline.tick()) {
            lostAtNs_ = start;
            return;
        }
        passTimes_.push_back(start);
        ++wokenBy_[static_cast<std::size_t>(control_.wokeBy())];
        for (EmulatedSocket* socket : sockets_) socket->readAll(start);
        clock_.advanceBy(options_.passWorkNs);

        EventLoopControl::Pass pass;
        pass.startNs = start;
        pass.inputChanged = pipeline.inputChanged();
        pass.windowOpen = options_.windowOpen;
        pass.forcesPlaying = options_.forcesPlaying;
        control_.afterPass(pass);
        const std::uint64_t deadline = std::min(control_.deadlineNs(), endNs);
        int ready = kWaitTimedOut;
        if (control_.canWait()) {
            ready = waitSet_.waitUntil(deadline);
        } else if (deadline > clock_.now()) {
            clock_.advanceTo(deadline);   // the sleep runLoop takes once the wait failed
        }
        if (ready == kWaitTimedOut && clock_.now() >= endNs) return;
        const EventLoopControl::Woke woke = control_.afterWait(ready, reasons_, clock_.now());
        if (woke.startedPolling && !pollingFromNs_) pollingFromNs_ = clock_.now();
        clock_.advanceBy(woke.sleepNs);
    }
}

std::uint64_t PushLoopRig::passesBetween(const std::uint64_t fromNs, const std::uint64_t toNs) const {
    const auto first = std::lower_bound(passTimes_.begin(), passTimes_.end(), fromNs);
    const auto last = std::lower_bound(passTimes_.begin(), passTimes_.end(), toNs);
    return static_cast<std::uint64_t>(last - first);
}

std::vector<std::uint64_t> PushLoopRig::latencies(const EmulatedPushDevice& device, const std::uint64_t untilNs) const {
    std::vector<std::uint64_t> result;
    for (const std::uint64_t change : device.changeTimes()) {
        if (change >= untilNs) break;
        const auto pass = std::lower_bound(passTimes_.begin(), passTimes_.end(), change);
        result.push_back(pass == passTimes_.end() ? kNever : *pass - change);
    }
    return result;
}

std::uint64_t PushLoopRig::maxLatency(const EmulatedPushDevice& device, const std::uint64_t fromNs,
                                      const std::uint64_t untilNs) const {
    std::uint64_t worst = 0;
    for (const std::uint64_t change : device.changeTimes()) {
        if (change < fromNs) continue;
        if (change >= untilNs) break;
        const auto pass = std::lower_bound(passTimes_.begin(), passTimes_.end(), change);
        worst = std::max(worst, pass == passTimes_.end() ? kNever : *pass - change);
    }
    return worst;
}

std::uint64_t PushLoopRig::minPassGap() const {
    std::uint64_t gap = kNever;
    for (std::size_t i = 1; i < passTimes_.size(); ++i) gap = std::min(gap, passTimes_[i] - passTimes_[i - 1]);
    return gap;
}

}  // namespace wheelbri::emulators
