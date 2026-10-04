#include "FfbForwarder.hpp"

#include "DiFormat.hpp"
#include "ShimConfig.hpp"
#include "ShimLogging.hpp"

namespace wheelbri::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.ffb");
    return instance;
}

constexpr auto kKeepAlive = std::chrono::milliseconds(100);
constexpr auto kRefresh = std::chrono::milliseconds(250);

}  // namespace

FfbForwarder& FfbForwarder::instance() {
    // Never destroyed (see StateFeed::instance): no socket closing and WSACleanup under the loader lock.
    static FfbForwarder* const forwarder = new FfbForwarder;
    return *forwarder;
}

void FfbForwarder::ensureSender() {
    if (senderTried_) {
        return;
    }
    senderTried_ = true;
    sender_ = UdpLoopbackSender::create(shimConfig().ffbPort);
    if (sender_ == nullptr) {
        WE_LOG_ERROR(log(), "cannot create the force feedback socket; the game's effects will not be felt");
        return;
    }
    WE_LOG_INFO(log(), "game force feedback goes to wheelbri-service on port {}", shimConfig().ffbPort);
    FfbPacket reset;
    reset.kind = FfbPacket::Kind::StopAll;   // clear whatever an earlier game session left behind
    send(reset, Clock::now());
}

void FfbForwarder::send(const FfbPacket& packet, const Clock::time_point now) {
    ensureSender();
    if (sender_ == nullptr) {
        return;
    }
    FfbPacket numbered = packet;
    numbered.sequence = ++sequence_;
    sender_->send(encode(numbered));
    lastSend_ = now;
}

bool FfbForwarder::effectivelyPlaying(const Slot& slot, const Clock::time_point now) const {
    if (!slot.inUse || !slot.started || paused_ || !actuatorsOn_) {
        return false;
    }
    const auto elapsedUs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - slot.startedAt).count());
    if (elapsedUs < slot.startDelayUs) {
        return false;
    }
    return !slot.lengthUs || elapsedUs - slot.startDelayUs < *slot.lengthUs;
}

void FfbForwarder::sendSlot(const std::uint8_t index, const Clock::time_point now, const bool force) {
    Slot& slot = slots_[index];
    const bool playing = effectivelyPlaying(slot, now);
    FfbEffect effect = slot.effect;
    effect.gain *= deviceGain_;
    const bool changed = playing != slot.sentPlaying || (playing && slot.sentEffect != effect);
    if (!changed && !force) {
        return;
    }
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = index;
    packet.playing = playing;
    packet.effect = effect;
    send(packet, now);
    if (playing != slot.sentPlaying) {
        // A game may restart a short effect every frame: sampled, the shim flushes every line to disk.
        static logging::CallSampler sampler(50, 500);
        if (const auto call = sampler.next(); call != 0) {
            WE_LOG_DEBUG(log(), "slot {}: {} (#{})", index, playing ? "playing" : "stopped", call);
        }
    }
    slot.sentPlaying = playing;
    slot.sentEffect = effect;
}

bool FfbForwarder::holdsLocked(const Allocation& a) const {
    return a.slot < slots_.size() && slots_[a.slot].inUse && slots_[a.slot].ticket == a.ticket;
}

bool FfbForwarder::holds(const Allocation& a) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return holdsLocked(a);
}

std::optional<FfbForwarder::Allocation> FfbForwarder::allocate(const void* const owner) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].inUse) {
            slots_[i] = Slot{};
            slots_[i].inUse = true;
            slots_[i].owner = owner;
            slots_[i].ticket = ++nextTicket_;
            return Allocation{static_cast<std::uint8_t>(i), slots_[i].ticket};
        }
    }
    static logging::RateLimiter limiter(std::chrono::seconds(5));
    WE_LOG_WARN_LIMITED(log(), limiter, "all {} force feedback slots in use; the game's next effect is refused",
                        slots_.size());
    return std::nullopt;
}

void FfbForwarder::release(const Allocation& a) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!holdsLocked(a)) return;   // already freed (a reset), maybe another effect's now
    slots_[a.slot].started = false;
    sendSlot(a.slot, Clock::now(), false);
    slots_[a.slot] = Slot{};
}

void FfbForwarder::setEffect(const Allocation& a, const FfbEffect& effect) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!holdsLocked(a)) return;
    slots_[a.slot].effect = effect;
    sendSlot(a.slot, Clock::now(), false);
}

void FfbForwarder::start(const Allocation& a, const std::optional<std::uint64_t> durationUs,
                         const std::optional<std::uint32_t> iterations, const std::uint64_t startDelayUs,
                         const bool solo) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!holdsLocked(a)) return;
    const std::uint8_t slot = a.slot;
    const auto now = Clock::now();
    if (solo) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (i != slot && slots_[i].inUse && slots_[i].owner == slots_[slot].owner) {
                slots_[i].started = false;
                sendSlot(static_cast<std::uint8_t>(i), now, false);
            }
        }
    }
    Slot& s = slots_[slot];
    s.started = true;
    s.startedAt = now;
    s.startDelayUs = startDelayUs;
    if (durationUs && iterations) {
        s.lengthUs = *durationUs * *iterations;
    } else {
        s.lengthUs.reset();
    }
    sendSlot(slot, now, false);
}

void FfbForwarder::stop(const Allocation& a) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!holdsLocked(a)) return;
    slots_[a.slot].started = false;
    sendSlot(a.slot, Clock::now(), false);
}

bool FfbForwarder::isPlaying(const Allocation& a) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return holdsLocked(a) && effectivelyPlaying(slots_[a.slot], Clock::now());
}

void FfbForwarder::stopAll(const void* const owner) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    bool othersStarted = false;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (slot.inUse && slot.owner != owner) {
            othersStarted = othersStarted || slot.started;
            continue;
        }
        slot.started = false;
        if (slot.inUse) {
            sendSlot(static_cast<std::uint8_t>(i), now, false);
        }
    }
    // The service's StopAll ends every slot: only when no other device of the game has effects started.
    if (!othersStarted) {
        FfbPacket packet;
        packet.kind = FfbPacket::Kind::StopAll;
        send(packet, now);
    }
}

void FfbForwarder::reset(const void* const owner) {
    stopAll(owner);
    const std::lock_guard<std::mutex> lock(mutex_);
    // DirectInput unloads the device's effects on a reset: their slots are free again (an effect that is used
    // again takes a new one, see EmulatedEffect::ensureSlot). Without this, a game that resets between races and
    // creates new effects ran out of slots.
    for (Slot& slot : slots_) {
        if (slot.inUse && slot.owner == owner) slot = Slot{};
    }
    paused_ = false;
    actuatorsOn_ = true;
}

void FfbForwarder::setPaused(const bool paused) {
    const std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
    const auto now = Clock::now();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].inUse) {
            sendSlot(static_cast<std::uint8_t>(i), now, false);
        }
    }
}

void FfbForwarder::setActuators(const bool on) {
    const std::lock_guard<std::mutex> lock(mutex_);
    actuatorsOn_ = on;
    const auto now = Clock::now();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].inUse) {
            sendSlot(static_cast<std::uint8_t>(i), now, false);
        }
    }
}

void FfbForwarder::setDeviceGain(const float gain) {
    const std::lock_guard<std::mutex> lock(mutex_);
    deviceGain_ = gain < 0.0F ? 0.0F : (gain > 1.0F ? 1.0F : gain);
    const auto now = Clock::now();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].inUse) {
            sendSlot(static_cast<std::uint8_t>(i), now, false);
        }
    }
}

float FfbForwarder::deviceGain() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return deviceGain_;
}

std::uint32_t FfbForwarder::stateFlags() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    bool any = false;
    bool anyPlaying = false;
    for (const Slot& slot : slots_) {
        any = any || slot.inUse;
        anyPlaying = anyPlaying || effectivelyPlaying(slot, now);
    }
    std::uint32_t flags = DIGFFS_POWERON | DIGFFS_SAFETYSWITCHON | DIGFFS_USERFFSWITCHON;
    flags |= actuatorsOn_ ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF;
    flags |= paused_ ? DIGFFS_PAUSED : 0;
    flags |= anyPlaying ? 0 : DIGFFS_STOPPED;
    flags |= any ? 0 : DIGFFS_EMPTY;
    return flags;
}

void FfbForwarder::tick() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    const bool refresh = now - lastRefresh_ >= kRefresh;
    if (refresh) {
        lastRefresh_ = now;
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        // Changes (also effects whose duration ran out or whose start delay has passed), and on a refresh every
        // slot, quiet and free ones too: a lost "stop" would otherwise leave the force on in the service, whose
        // watchdog the keep-alives below keep fed.
        if (slots_[i].inUse || refresh) {
            sendSlot(static_cast<std::uint8_t>(i), now, refresh);
        }
    }
    if (now - lastSend_ >= kKeepAlive) {
        send(FfbPacket{}, now);   // KeepAlive
    }
}

}  // namespace wheelbri::shim
