#include "wheelbri/HidPpFfb.hpp"

#include "wheelbri/HidPid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>

namespace wheelbri {

namespace {

constexpr std::uint8_t kAutostart = 0x80;
constexpr std::uint8_t kStatePlay = 0x02;

std::uint16_t be16(const std::span<const std::uint8_t> p, const std::size_t at) {
    return at + 1 < p.size() ? static_cast<std::uint16_t>((p[at] << 8) | p[at + 1]) : 0;
}

// A signed level: +-0x7FFF = +-1.
float signedLevel(const std::span<const std::uint8_t> p, const std::size_t at) {
    return std::clamp(static_cast<float>(static_cast<std::int16_t>(be16(p, at))) / 32767.0F, -1.0F, 1.0F);
}

// A saturation or deadband: 0..0x7FFF = 0..1.
float share(const std::span<const std::uint8_t> p, const std::size_t at) {
    return std::clamp(static_cast<float>(be16(p, at)) / 32767.0F, 0.0F, 1.0F);
}

std::optional<FfbEnvelope> envelopeAt(const std::span<const std::uint8_t> p, const std::size_t at) {
    FfbEnvelope e;
    e.attackLevel = (at < p.size() ? p[at] : 0) / 255.0F;
    e.attackMs = be16(p, at + 1);
    e.fadeLevel = (at + 3 < p.size() ? p[at + 3] : 0) / 255.0F;
    e.fadeMs = be16(p, at + 4);
    if (e.attackMs == 0.0F && e.fadeMs == 0.0F) return std::nullopt;
    return e;
}

// Not Ffb.cpp's helpers: the driver compiles this file without the rest of core (as HidPid).
bool isCondition(const FfbEffectType type) {
    return type == FfbEffectType::Spring || type == FfbEffectType::Damper || type == FfbEffectType::Friction ||
           type == FfbEffectType::Inertia;
}

const char* nameOf(const std::uint8_t type) {
    constexpr const char* kNames[] = {"constant", "sine", "square", "triangle", "saw up", "saw down", "spring",
                                      "damper", "friction", "inertia", "ramp"};
    return type < std::size(kNames) ? kNames[type] : "?";
}

std::optional<FfbEffectType> typeOf(const std::uint8_t type) {
    switch (type) {
        case 0x00: return FfbEffectType::Constant;
        case 0x01: return FfbEffectType::Sine;
        case 0x02: return FfbEffectType::Square;
        case 0x03: return FfbEffectType::Triangle;
        case 0x04: return FfbEffectType::SawtoothUp;
        case 0x05: return FfbEffectType::SawtoothDown;
        case 0x06: return FfbEffectType::Spring;
        case 0x07: return FfbEffectType::Damper;
        case 0x08: return FfbEffectType::Friction;
        case 0x09: return FfbEffectType::Inertia;
        case 0x0A: return FfbEffectType::Constant;   // a ramp: a constant force with an end level
        default: return std::nullopt;
    }
}

std::string describe(const std::uint8_t slot, const std::uint8_t type, const FfbEffect& e, const FfbShape& shape,
                     const std::int64_t durationMs) {
    char text[200];
    if (isCondition(e.type)) {
        std::snprintf(text, sizeof(text), "slot %u %s k %.2f/%.2f sat %.2f/%.2f deadband %.2f centre %.2f",
                      static_cast<unsigned>(slot), nameOf(type), e.condition.negativeCoefficient,
                      e.condition.positiveCoefficient, e.condition.negativeSaturation, e.condition.positiveSaturation,
                      e.condition.deadband, e.condition.center);
    } else if (e.type == FfbEffectType::Constant) {
        std::snprintf(text, sizeof(text), "slot %u %s level %.3f to %.3f", static_cast<unsigned>(slot), nameOf(type),
                      e.constantLevel, shape.rampEndLevel.value_or(e.constantLevel));
    } else {
        std::snprintf(text, sizeof(text), "slot %u %s magnitude %.2f offset %.2f period %.0f ms",
                      static_cast<unsigned>(slot), nameOf(type), e.periodic.magnitude, e.periodic.offset,
                      e.periodic.periodMs);
    }
    std::string line = text;
    if (durationMs > 0) line += ", " + std::to_string(durationMs) + " ms";
    if (shape.envelope) line += ", envelope";
    return line;
}

}  // namespace

HidPpFfb::HidPpFfb(Sender send, Logger log) : send_(std::move(send)), log_(std::move(log)) {}

void HidPpFfb::request(const std::int64_t nowMs) {
    if (requests_ == 0 || nowMs - lastRequestMs_ > HidPid::kStreamGapMs) streamStartMs_ = nowMs;
    ++requests_;
    lastRequestMs_ = nowMs;
    if (quiet_) {
        quiet_ = false;
        if (log_) log_(LogLevel::Info, "hid++ ffb: the game sends again: keep-alives back");
    }
}

std::uint8_t HidPpFfb::download(const std::span<const std::uint8_t> p, const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    request(nowMs);
    if (p.size() < 6) return 0;
    const std::uint8_t typeByte = p[1];
    const auto type = typeOf(static_cast<std::uint8_t>(typeByte & ~kAutostart));
    if (!type) {
        if (log_) log_(LogLevel::Warn, "hid++ ffb: effect type " + std::to_string(typeByte) + " not known: not played");
        return 0;
    }
    std::uint8_t slot = p[0];
    if (slot > kSlots) return 0;
    if (slot == 0) {
        for (std::uint8_t s = 1; s <= kSlots; ++s) {
            if (!effects_[s].loaded) {
                slot = s;
                break;
            }
        }
        if (slot == 0) return 0;
    }
    effectsSeen_ = true;
    Effect& e = effects_[slot];
    const bool update = e.loaded;
    FfbEffect effect;
    effect.type = *type;
    FfbShape shape;
    switch (typeByte & ~kAutostart) {
        case 0x00:
            effect.constantLevel = signedLevel(p, 6);
            shape.envelope = envelopeAt(p, 8);
            break;
        case 0x0A:
            effect.constantLevel = signedLevel(p, 6);
            shape.rampEndLevel = signedLevel(p, 8);
            shape.envelope = envelopeAt(p, 10);
            break;
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
            effect.condition.negativeSaturation = share(p, 6);
            effect.condition.negativeCoefficient = signedLevel(p, 8);
            effect.condition.deadband = share(p, 10);
            effect.condition.center = signedLevel(p, 12);
            effect.condition.positiveCoefficient = signedLevel(p, 14);
            effect.condition.positiveSaturation = share(p, 16);
            break;
        default: {
            const float magnitude = signedLevel(p, 6);
            effect.periodic.magnitude = std::fabs(magnitude);
            effect.periodic.offset = signedLevel(p, 8);
            effect.periodic.periodMs = be16(p, 10);
            effect.periodic.phaseDeg = std::fmod(be16(p, 12) * 360.0F / 65536.0F + (magnitude < 0 ? 180.0F : 0.0F),
                                                 360.0F);
            shape.envelope = envelopeAt(p, 14);
            break;
        }
    }
    e.loaded = true;
    e.effect = effect;
    e.shape = shape;
    e.durationMs = be16(p, 2);
    e.delayMs = be16(p, 4);
    if (log_) {
        const bool shown = shown_ < kShownDownloads;
        if (shown) ++shown_;
        const auto kind = static_cast<std::uint8_t>(typeByte & ~kAutostart);
        log_(shown ? LogLevel::Info : LogLevel::Debug,
             std::string("hid++ ffb: ") + (update ? "update " : "new ") +
                 describe(slot, kind, effect, shape, e.durationMs) +
                 ((typeByte & kAutostart) ? ", starts at once" : ""));
    }
    if ((typeByte & kAutostart) && !e.playing) play(slot, nowMs);
    sendChanges(nowMs, false);
    return slot;
}

void HidPpFfb::setState(const std::uint8_t slot, const std::uint8_t state, const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    request(nowMs);
    if (slot == 0 || slot > kSlots || !effects_[slot].loaded) return;
    if (state == kStatePlay) {
        play(slot, nowMs);
    } else if (state != 0) {   // stop or pause; 0 only asks
        stop(slot, nowMs);
    }
    sendChanges(nowMs, false);
}

void HidPpFfb::destroy(const std::uint8_t slot, const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    request(nowMs);
    if (slot == 0 || slot > kSlots) return;
    stop(slot, nowMs);
    sendChanges(nowMs, false);
    effects_[slot] = Effect{};
}

void HidPpFfb::resetAll(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    request(nowMs);
    effects_.fill(Effect{});
    owner_.fill(0);
    sent_.fill(std::nullopt);
    send(FfbPacket::Kind::StopAll, 0, false, FfbEffect{}, nowMs);
}

void HidPpFfb::setGain(const float gain, const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    request(nowMs);
    gain_ = std::clamp(gain, 0.0F, 1.0F);
    sendChanges(nowMs, false);
}

void HidPpFfb::play(const std::uint8_t slot, const std::int64_t nowMs) {
    Effect& e = effects_[slot];
    if (e.serviceSlot < 0) {
        const auto free = std::find(owner_.begin(), owner_.end(), std::uint8_t{0});
        if (free == owner_.end()) {
            if (log_ && !overflowNoted_) {
                log_(LogLevel::Warn, "hid++ ffb: more than " + std::to_string(kServiceSlots) +
                                         " effects at once: slot " + std::to_string(slot) + " not played");
            }
            overflowNoted_ = true;
            return;
        }
        *free = slot;
        e.serviceSlot = static_cast<int>(free - owner_.begin());
    }
    e.playing = true;
    e.startedAtMs = nowMs;
}

void HidPpFfb::stop(const std::uint8_t slot, const std::int64_t /*nowMs*/) {
    Effect& e = effects_[slot];
    e.playing = false;
}

std::optional<FfbEffect> HidPpFfb::now(const Effect& e, const std::int64_t nowMs) const {
    if (!e.playing) return std::nullopt;
    const std::int64_t elapsed = nowMs - e.startedAtMs - e.delayMs;
    if (elapsed < 0) return std::nullopt;
    if (e.durationMs > 0 && elapsed >= e.durationMs) return std::nullopt;
    const double duration = e.durationMs > 0 ? static_cast<double>(e.durationMs) : kUntilStopped;
    FfbEffect out = shapeAt(e.effect, e.shape, static_cast<double>(elapsed), duration);
    out.gain *= gain_;
    return out;
}

void HidPpFfb::sendChanges(const std::int64_t nowMs, const bool refresh) {
    for (std::size_t s = 0; s < kServiceSlots; ++s) {
        const std::uint8_t slot = owner_[s];
        if (slot == 0) continue;
        Effect& e = effects_[slot];
        // A play that ran its duration is over.
        if (e.playing && e.durationMs > 0 && nowMs - e.startedAtMs - e.delayMs >= e.durationMs) e.playing = false;
        const std::optional<FfbEffect> effect = now(e, nowMs);
        const bool isPlaying = effect.has_value();
        const FfbEffect value = effect.value_or(e.effect);
        const auto& last = sent_[s];
        const bool changed = !last || last->playing != isPlaying || (isPlaying && !(last->effect == value));
        if (changed || (refresh && last && last->playing)) {
            send(FfbPacket::Kind::SlotState, static_cast<std::uint8_t>(s), isPlaying, value, nowMs);
            sent_[s] = Sent{isPlaying, value};
        }
        // Stopped (and not just waiting for its delay): the service slot is free again.
        if (!e.playing) {
            owner_[s] = 0;
            e.serviceSlot = -1;
            overflowNoted_ = false;
        }
    }
}

void HidPpFfb::send(const FfbPacket::Kind kind, const std::uint8_t slot, const bool playing, const FfbEffect& effect,
                    const std::int64_t nowMs) {
    FfbPacket packet;
    packet.kind = kind;
    packet.slot = slot;
    packet.sequence = ++sequence_;
    packet.playing = playing;
    packet.effect = effect;
    if (send_) send_(packet);
    lastSendMs_ = nowMs;
    ++packetsSent_;
}

bool HidPpFfb::pushing(const float above) const {
    for (std::size_t s = 0; s < kServiceSlots; ++s) {
        const auto& last = sent_[s];
        if (!last || !last->playing || isCondition(last->effect.type)) continue;
        const FfbEffect& e = last->effect;
        const float level = e.type == FfbEffectType::Constant
                                ? std::fabs(e.constantLevel)
                                : std::fabs(e.periodic.magnitude) + std::fabs(e.periodic.offset);
        if (level * e.gain > above) return true;
    }
    return false;
}

void HidPpFfb::tick(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    // Keep-alives only once an effect came: G HUB setting the gain is not a game (HidPid does the same).
    if (!effectsSeen_) return;
    const bool streamed = lastRequestMs_ - streamStartMs_ >= HidPid::kStreamMinMs;
    if (!quiet_ && streamed && nowMs - lastRequestMs_ >= HidPid::kStreamQuietMs &&
        pushing(HidPid::kStreamPushingAbove)) {
        quiet_ = true;
        if (log_) {
            log_(LogLevel::Info, "hid++ ffb: the game streamed force feedback and then stopped (closed, crashed or "
                                 "paused?): keep-alives stopped, so the service stops the forces");
        }
    }
    if (!quiet_ && nowMs - lastRequestMs_ >= HidPid::kGameQuietMs && pushing(HidPid::kQuietPushingAbove)) {
        quiet_ = true;
        if (log_) {
            log_(LogLevel::Warn, "hid++ ffb: nothing from the game for " + std::to_string(HidPid::kGameQuietMs / 1000) +
                                     " s while a pushing force plays: keep-alives stopped");
        }
    }
    const bool refresh = !quiet_ && nowMs - lastRefreshMs_ >= kRefreshMs;
    if (refresh) lastRefreshMs_ = nowMs;
    sendChanges(nowMs, refresh);
    if (!quiet_ && nowMs - lastSendMs_ >= kKeepAliveMs) send(FfbPacket::Kind::KeepAlive, 0, false, FfbEffect{}, nowMs);
}

void HidPpFfb::stopAll(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    for (Effect& e : effects_) e.playing = false;
    owner_.fill(0);
    sent_.fill(std::nullopt);
    if (effectsSeen_) send(FfbPacket::Kind::StopAll, 0, false, FfbEffect{}, nowMs);
}

std::uint64_t HidPpFfb::packetsSent() const {
    std::lock_guard lock(mutex_);
    return packetsSent_;
}

}  // namespace wheelbri
