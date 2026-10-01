#include "wheelemu/FfbController.hpp"

#include "wheelemu/Logging.hpp"

#include <cmath>

namespace wheelemu {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.ffb");
    return logger;
}

const char* typeName(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return "constant";
        case FfbEffectType::Spring: return "spring";
        case FfbEffectType::Damper: return "damper";
        case FfbEffectType::Friction: return "friction";
        case FfbEffectType::Inertia: return "inertia";
        case FfbEffectType::Sine: return "sine";
        case FfbEffectType::Square: return "square";
        case FfbEffectType::Triangle: return "triangle";
        case FfbEffectType::SawtoothUp: return "sawtooth-up";
        case FfbEffectType::SawtoothDown: return "sawtooth-down";
    }
    return "?";
}

bool closeTo(const float a, const float b) { return std::abs(a - b) < 1e-3F; }

}  // namespace

FfbController::FfbController(IFfbSink& sink, const FfbSafetyConfig& safety, const Timing timing)
    : sink_(sink), safety_(safety), timing_(timing), usage_(safety) {
    limiters_.reserve(slots_.size());
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        limiters_.emplace_back(safety_);
    }
}

bool FfbController::differs(const FfbEffect& a, const FfbEffect& b) {
    return a.type != b.type || !closeTo(a.gain, b.gain) || !closeTo(a.constantLevel, b.constantLevel) ||
           !closeTo(a.condition.center, b.condition.center) || !closeTo(a.condition.deadband, b.condition.deadband) ||
           !closeTo(a.condition.positiveCoefficient, b.condition.positiveCoefficient) ||
           !closeTo(a.condition.negativeCoefficient, b.condition.negativeCoefficient) ||
           !closeTo(a.condition.positiveSaturation, b.condition.positiveSaturation) ||
           !closeTo(a.condition.negativeSaturation, b.condition.negativeSaturation) ||
           !closeTo(a.periodic.magnitude, b.periodic.magnitude) || !closeTo(a.periodic.offset, b.periodic.offset) ||
           !closeTo(a.periodic.periodMs, b.periodic.periodMs) || !closeTo(a.periodic.phaseDeg, b.periodic.phaseDeg);
}

void FfbController::onPacket(const FfbPacket& packet, const std::uint64_t nowNs) {
    ++stats_.packets;
    lastPacketNs_ = nowNs;
    if (watchdogTripped_) {
        watchdogTripped_ = false;
        WE_LOG_INFO(log(), "ffb: game force feedback is back");
    }

    switch (packet.kind) {
        case FfbPacket::Kind::KeepAlive:
            return;
        case FfbPacket::Kind::StopAll:
            WE_LOG_DEBUG(log(), "ffb: game stopped all effects");
            for (Slot& slot : slots_) {
                slot.wanted = false;
            }
            return;
        case FfbPacket::Kind::SlotState:
            break;
    }
    if (packet.slot >= slots_.size()) {
        static logging::RateLimiter limiter(std::chrono::seconds(10));
        WE_LOG_WARN_LIMITED(log(), limiter, "ffb: packet for slot {} ignored (only {} slots)", packet.slot,
                            slots_.size());
        return;
    }
    Slot& slot = slots_[packet.slot];
    if (slot.wanted != packet.playing) {
        WE_LOG_DEBUG(log(), "ffb: slot {} {} {}", packet.slot, packet.playing ? "starts" : "stops",
                     typeName(packet.effect.type));
    }
    slot.wanted = packet.playing;
    slot.requested = packet.effect;
}

void FfbController::update(const std::uint64_t nowNs) {
    if (lastPacketNs_ && !watchdogTripped_ && nowNs - *lastPacketNs_ > timing_.watchdogNs) {
        watchdogTripped_ = true;
        bool anyWanted = false;
        for (Slot& slot : slots_) {
            anyWanted = anyWanted || slot.wanted;
            slot.wanted = false;
        }
        if (anyWanted) {
            ++stats_.watchdogStops;
            WE_LOG_WARN(log(), "ffb: no word from the game for {} ms, all forces stopped",
                        timing_.watchdogNs / 1'000'000);
        }
    }

    std::size_t active = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.wanted) {
            if (slot.playing) {
                sink_.stop(i);
                slot.playing = false;
                slot.lastSent.reset();
                limiters_[i].reset();
            }
            continue;
        }
        ++active;
        const bool starting = !slot.playing;
        if (!starting && nowNs - slot.lastSentNs < timing_.minUpdateNs) {
            continue;
        }
        const float dt = starting ? 0.0F : static_cast<float>(nowNs - slot.lastUpdateNs) / 1e9F;
        slot.lastUpdateNs = nowNs;
        const FfbEffect limited = limiters_[i].apply(slot.requested, dt);
        if (!starting && slot.lastSent && !differs(limited, *slot.lastSent)) {
            continue;
        }
        ++stats_.plays;
        if (sink_.play(i, limited)) {
            slot.playing = true;
            slot.lastSent = limited;
            slot.lastSentNs = nowNs;
        } else {
            ++stats_.playFailures;
            slot.wanted = false;   // do not retry every tick; the game will send it again if it cares
        }
    }
    stats_.activeSlots = active;

    std::array<FfbUsage::SlotSample, IFfbSink::kMaxSlots> samples{};
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot& slot = slots_[i];
        if (slot.playing && slot.lastSent) {
            samples[i] = {true, slot.requested, *slot.lastSent};
        }
    }
    const std::uint64_t dt = lastUsageNs_ && nowNs > *lastUsageNs_ ? nowNs - *lastUsageNs_ : 0;
    lastUsageNs_ = nowNs;
    usage_.record(dt, lastPacketNs_.has_value() && !watchdogTripped_, samples);
}

void FfbController::stopAll() {
    sink_.stopAll();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i] = Slot{};
        limiters_[i].reset();
    }
}

}  // namespace wheelemu
