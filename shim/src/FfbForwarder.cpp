#include "FfbForwarder.hpp"

#include "DiFormat.hpp"
#include "ShimConfig.hpp"
#include "ShimLogging.hpp"

namespace wheelemu::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.ffb");
    return instance;
}

constexpr auto kKeepAlive = std::chrono::milliseconds(100);
constexpr auto kRefresh = std::chrono::milliseconds(250);

}  // namespace

FfbForwarder& FfbForwarder::instance() {
    static FfbForwarder forwarder;
    return forwarder;
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
    WE_LOG_INFO(log(), "game force feedback goes to wheelemu-service on port {}", shimConfig().ffbPort);
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
        WE_LOG_DEBUG(log(), "slot {}: {}", index, playing ? "playing" : "stopped");
    }
    slot.sentPlaying = playing;
    slot.sentEffect = effect;
}

std::optional<std::uint8_t> FfbForwarder::allocate() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].inUse) {
            slots_[i] = Slot{};
            slots_[i].inUse = true;
            return static_cast<std::uint8_t>(i);
        }
    }
    WE_LOG_WARN(log(), "all {} force feedback slots in use; the game's next effect is refused", slots_.size());
    return std::nullopt;
}

void FfbForwarder::release(const std::uint8_t slot) {
    const std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot].started = false;
    sendSlot(slot, Clock::now(), false);
    slots_[slot] = Slot{};
}

void FfbForwarder::setEffect(const std::uint8_t slot, const FfbEffect& effect) {
    const std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot].effect = effect;
    sendSlot(slot, Clock::now(), false);
}

void FfbForwarder::start(const std::uint8_t slot, const std::optional<std::uint64_t> durationUs,
                         const std::optional<std::uint32_t> iterations, const std::uint64_t startDelayUs,
                         const bool solo) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    if (solo) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (i != slot && slots_[i].inUse) {
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

void FfbForwarder::stop(const std::uint8_t slot) {
    const std::lock_guard<std::mutex> lock(mutex_);
    slots_[slot].started = false;
    sendSlot(slot, Clock::now(), false);
}

bool FfbForwarder::isPlaying(const std::uint8_t slot) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return effectivelyPlaying(slots_[slot], Clock::now());
}

void FfbForwarder::stopAll() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].started = false;
        if (slots_[i].inUse) {
            sendSlot(static_cast<std::uint8_t>(i), now, false);
        }
    }
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::StopAll;
    send(packet, now);
}

void FfbForwarder::reset() {
    stopAll();
    const std::lock_guard<std::mutex> lock(mutex_);
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
        // Also catches effects whose duration ran out or whose start delay has passed.
        if (slots_[i].inUse) {
            sendSlot(static_cast<std::uint8_t>(i), now, refresh && slots_[i].sentPlaying);
        }
    }
    if (now - lastSend_ >= kKeepAlive) {
        send(FfbPacket{}, now);   // KeepAlive
    }
}

}  // namespace wheelemu::shim
