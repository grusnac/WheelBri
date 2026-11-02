#include "wheelbri/FfbController.hpp"

#include "wheelbri/CentringSpring.hpp"
#include "wheelbri/FfbGroups.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace wheelbri {

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

std::uint64_t frozenNsFor(const float frozen) {
    // Not a number: the default; negative: never.
    const float seconds =
        std::isfinite(frozen) ? std::clamp(frozen, 0.0F, 3600.0F) : FfbSafetyConfig{}.frozenForceSeconds;
    return static_cast<std::uint64_t>(static_cast<double>(seconds) * 1e9);
}

bool isPeriodic(const FfbEffectType type) { return ffbGroupOf(type) == FfbGroup::Periodic; }

// The steady push in an effect, signed: a constant force's level, a periodic effect's offset; none in a condition.
float pushOf(const FfbEffect& effect) {
    if (effect.type == FfbEffectType::Constant) return effect.constantLevel;
    if (isPeriodic(effect.type)) return effect.periodic.offset;
    return 0.0F;
}

// The most force an effect can put on the wheel at any moment (of full scale), for the cap on all slots together:
// a constant force's level, a periodic effect's offset and magnitude; conditions answer the wheel's own movement and
// are capped by their saturations, so they are not counted.
float peakOf(const FfbEffect& effect) {
    if (effect.type == FfbEffectType::Constant) return std::abs(effect.constantLevel);
    if (isPeriodic(effect.type)) return std::abs(effect.periodic.offset) + std::abs(effect.periodic.magnitude);
    return 0.0F;
}

// `effect`'s pushing forces times `scale` (0..1).
FfbEffect scaledDown(const FfbEffect& effect, const float scale) {
    FfbEffect out = effect;
    out.constantLevel *= scale;
    out.periodic.offset *= scale;
    out.periodic.magnitude *= scale;
    return out;
}

template <std::size_t... I>
std::array<FfbSafetyLimiter, sizeof...(I)> makeLimiters(const FfbSafetyConfig& safety, std::index_sequence<I...>) {
    return {((void)I, FfbSafetyLimiter(safety))...};
}

}  // namespace

FfbController::FfbController(IFfbSink& sink, const FfbSafetyConfig& safety, const Timing timing)
    : sink_(sink),
      safety_(safety),
      timing_(timing),
      limiters_(makeLimiters(safety, std::make_index_sequence<IFfbSink::kMaxSlots>{})),
      springLimiter_(safety),
      usage_(safety) {
    frozenNs_ = frozenNsFor(safety.frozenForceSeconds);
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
                slot.loaded = false;   // "stop all" or "reset": what comes next is a new set of effects
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
    // A new push, or one the game changed (a restart with the same push is not a change: still frozen).
    const FfbEffect next = screenFullPush(packet.effect, nowNs);
    if (!slot.loaded || next.type != slot.requested.type || !closeTo(next.gain, slot.requested.gain) ||
        !closeTo(next.constantLevel, slot.requested.constantLevel) ||
        !closeTo(next.periodic.offset, slot.requested.periodic.offset)) {
        slot.pushSinceNs = nowNs;
    }
    if (slot.wanted != packet.playing) {
        WE_LOG_DEBUG(log(), "ffb: slot {} {} {}", packet.slot, packet.playing ? "starts" : "stops",
                     typeName(packet.effect.type));
    }
    slot.wanted = packet.playing;
    slot.loaded = true;
    slot.requested = next;
}

FfbEffect FfbController::screened(const FfbEffect& effect, const bool realForceRecent) {
    const bool full = std::abs(pushOf(effect)) >= kFullScaleAt;
    if (realForceRecent || !full) return effect;
    FfbEffect out = effect;
    if (effect.type == FfbEffectType::Constant) out.constantLevel = 0.0F;
    if (isPeriodic(effect.type)) out.periodic.offset = 0.0F;
    return out;
}

FfbEffect FfbController::screenFullPush(const FfbEffect& effect, const std::uint64_t nowNs) {
    if (effect.type != FfbEffectType::Constant && !isPeriodic(effect.type)) return effect;
    const float push = std::abs(pushOf(effect));
    if (push > 0.0F && push < kFullScaleAt) {
        // A real force: every full push from now on (for kRealForceWithinNs) is believed.
        lastRealForceNs_ = nowNs;
        if (ignoringLoneFullPush_) {
            ignoringLoneFullPush_ = false;
            WE_LOG_INFO(log(), "ffb: the game sends real forces again: its full pushes play as asked");
        }
        return effect;
    }
    if (!(push >= kFullScaleAt) || !safety_.ignoreLoneFullPush) return effect;
    const bool recent =
        lastRealForceNs_ && nowNs >= *lastRealForceNs_ && nowNs - *lastRealForceNs_ <= kRealForceWithinNs;
    if (recent) return effect;
    ++stats_.loneFullPushes;
    if (!ignoringLoneFullPush_) {
        ignoringLoneFullPush_ = true;
        static logging::RateLimiter limiter(std::chrono::minutes(1));
        WE_LOG_WARN_LIMITED(log(), limiter,
                            "ffb: the game asks for a full {} push with no real force for {:.0f} s around it: not a "
                            "force (Race Driver: GRID passes a broken number that Windows turns into a full push), "
                            "played as none until it sends forces between none and full again ([ffb] "
                            "ignore_lone_full_push = false turns this off)",
                            pushOf(effect) < 0.0F ? "left" : "right",
                            static_cast<double>(kRealForceWithinNs) / 1e9);
    }
    return screened(effect, false);
}

bool FfbController::lasting(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant:
        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia:
            return true;
        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown:
            break;
    }
    return false;
}

std::size_t FfbController::lastingLoaded() const {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.loaded && lasting(slot.requested.type)) ++count;
    }
    return count;
}

std::size_t FfbController::lastingWanted() const {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.loaded && slot.wanted && lasting(slot.requested.type)) ++count;
    }
    return count;
}

FfbEffect FfbController::tuned(const FfbEffect& effect, const float pushFade) {
    FfbEffect out = effect;
    if (effect.type == FfbEffectType::Constant) {
        out.constantLevel = effect.constantLevel * pushFade;
    } else if (isPeriodic(effect.type)) {
        out.periodic.magnitude = effect.periodic.magnitude * pushFade;
        out.periodic.offset = effect.periodic.offset * pushFade;
    }
    return out;
}

void FfbController::update(const std::uint64_t nowNs) {
    if (lastPacketNs_ && !watchdogTripped_ && nowNs - *lastPacketNs_ > timing_.watchdogNs) {
        watchdogTripped_ = true;
        bool anyWanted = false;
        for (Slot& slot : slots_) {
            anyWanted = anyWanted || slot.wanted;
            slot.wanted = false;
            slot.loaded = false;   // the game went quiet (crashed, closed): what it had loaded is not known any more
        }
        if (anyWanted) {
            ++stats_.watchdogStops;
            WE_LOG_WARN(log(), "ffb: no word from the game for {} ms, all forces stopped",
                        timing_.watchdogNs / 1'000'000);
        }
    }

    const float fade = frozenFadeAt(nowNs);
    std::size_t active = 0;
    // First every slot through its limiter (what each would play alone), then the cap on all of them together, then
    // what changed to the sink.
    std::array<bool, IFfbSink::kMaxSlots> fresh{};   // limited this pass (not held back by minUpdateNs)
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.wanted) {
            if (slot.playing) {
                sink_.stop(i);
                slot.playing = false;
                slot.lastSent.reset();
                limiters_[i].reset();
            }
            slot.lastLimited.reset();
            continue;
        }
        ++active;
        // Another kind of effect in the same slot (the game reused it, maybe after a stop that never arrived): the
        // limiter's state belongs to the old one, so it starts afresh, as a new effect does (ramping up from 0).
        const bool typeChanged = slot.playing && slot.lastSent && slot.lastSent->type != slot.requested.type;
        if (typeChanged) limiters_[i].reset();
        const bool starting = !slot.playing || typeChanged;
        if (!starting && nowNs - slot.lastSentNs < timing_.minUpdateNs && slot.lastLimited) {
            continue;
        }
        const float dt = starting ? 0.0F : static_cast<float>(nowNs - slot.lastUpdateNs) / 1e9F;
        slot.lastUpdateNs = nowNs;
        slot.lastLimited = limiters_[i].apply(tuned(slot.requested, fade), dt);
        fresh[i] = true;
    }

    // A cap below full scale holds for the sum, not only for each effect: two constant forces at the cap each would
    // put twice the cap on the wheel. Over it, every pushing force is scaled down by the same factor. At full scale
    // (the default, "no cap") the wheel itself is the limit, and the game's mix plays as it asks.
    float peak = 0.0F;
    for (const Slot& slot : slots_) {
        if (slot.lastLimited) peak += peakOf(*slot.lastLimited);
    }
    const float cap = safety_.maxMagnitude;
    const float scale = cap < 1.0F && peak > cap && peak > 0.0F ? std::max(cap, 0.0F) / peak : 1.0F;
    if (scale < 1.0F) {
        static logging::RateLimiter limiter(std::chrono::seconds(10));
        WE_LOG_WARN_LIMITED(log(), limiter,
                            "ffb: the effects together ask for {:.0f}% (cap {:.0f}%): all scaled by {:.2f}",
                            peak * 100.0F, cap * 100.0F, scale);
    }

    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& slot = slots_[i];
        if (!slot.lastLimited) continue;
        // A slot held back by minUpdateNs is sent anyway when the scale has come down since: a lower force never
        // waits.
        if (!fresh[i] && !(scale < slot.sentScale)) continue;
        const FfbEffect limited = scaledDown(*slot.lastLimited, scale);
        if (slot.playing && slot.lastSent && !differs(limited, *slot.lastSent)) {
            slot.sentScale = scale;
            continue;
        }
        ++stats_.plays;
        if (sink_.play(i, limited)) {
            slot.playing = true;
            slot.lastSent = limited;
            slot.lastSentNs = nowNs;
            slot.sentScale = scale;
        } else {
            ++stats_.playFailures;
            slot.wanted = false;   // do not retry every tick; the game will send it again if it cares
            slot.lastLimited.reset();
        }
    }
    stats_.activeSlots = active;
    updateCentringSpring();

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

float FfbController::frozenFadeAt(const std::uint64_t nowNs) {
    // The constant push frozen longest: the game paused its physics and kept sending the last force.
    std::optional<std::uint64_t> frozenAt;
    float level = 0.0F;
    if (frozenNs_ > 0) {
        for (const Slot& slot : slots_) {
            const FfbEffectType type = slot.requested.type;
            if (!slot.wanted || (type != FfbEffectType::Constant && !isPeriodic(type))) continue;
            // A periodic effect's offset is a push as a constant force is (its waveform alone is not).
            const float push = std::abs(pushOf(slot.requested) * slot.requested.gain);
            if (!(push > kFrozenPushAbove) || nowNs < slot.pushSinceNs || nowNs - slot.pushSinceNs < frozenNs_) {
                continue;
            }
            const std::uint64_t at = slot.pushSinceNs + frozenNs_;
            if (!frozenAt || at < *frozenAt) {
                frozenAt = at;
                level = push;
            }
        }
    }
    const float fade = frozenAt ? std::clamp(1.0F - static_cast<float>(nowNs - *frozenAt) /
                                                        static_cast<float>(kFrozenFadeNs), 0.0F, 1.0F)
                                : 1.0F;
    if (fade < 1.0F && frozenFade_ == 1.0F) {
        WE_LOG_INFO(log(), "ffb: the game has held a {:.0f}% push unchanged for {:.1f} s (paused, loading?): the "
                           "pushing forces fade out (springs and dampers stay); back when the game changes it",
                    std::min(level, 1.0F) * 100.0F, static_cast<double>(frozenNs_) / 1e9);
    } else if (fade == 1.0F && frozenFade_ < 1.0F) {
        WE_LOG_INFO(log(), "ffb: the game changed its push again: forces back");
    }
    frozenFade_ = fade;
    return fade;
}

void FfbController::setCentringSpring(const float percent) {
    const float clamped = std::isfinite(percent) ? std::clamp(percent, 0.0F, kCentringSpringMaxPercent) : 0.0F;
    springRefused_ = false;
    if (clamped == springPercent_) return;
    if (clamped > 0.0F) {
        WE_LOG_INFO(log(), "ffb: centring spring {:.0f}% (the target's own, as its driver had it): a spring around the "
                           "centre on top of the game's forces, through the limiter",
                    clamped);
    } else {
        WE_LOG_INFO(log(), "ffb: centring spring off");
    }
    springPercent_ = clamped;
}

void FfbController::updateCentringSpring() {
    if (springPercent_ <= 0.0F || springRefused_) {
        if (springPlaying_) {
            sink_.stop(IFfbSink::kCentringSpringSlot);
            springPlaying_ = false;
            springSent_.reset();
        }
        return;
    }
    // A condition: the limiter keeps no state for it, so dt does not matter.
    const FfbEffect limited = springLimiter_.apply(centringSpringEffect(springPercent_), 0.0F);
    if (springPlaying_ && springSent_ && !differs(limited, *springSent_)) return;
    ++stats_.plays;
    if (sink_.play(IFfbSink::kCentringSpringSlot, limited)) {
        springPlaying_ = true;
        springSent_ = limited;
        return;
    }
    ++stats_.playFailures;
    springRefused_ = true;   // not every tick: the wheel said no
    springPlaying_ = false;
    springSent_.reset();
    WE_LOG_WARN(log(), "ffb: the wheel refused the centring spring ({:.0f}%): none until the next Start",
                springPercent_);
}

void FfbController::stopAll() {
    sink_.stopAll();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slots_[i] = Slot{};
        limiters_[i].reset();
    }
    springPlaying_ = false;   // stopped with the rest; the next update() plays it again
    springSent_.reset();
}

}  // namespace wheelbri
