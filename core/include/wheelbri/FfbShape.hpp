// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Ffb.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// How an effect changes while it plays: its envelope (attack and fade) and, for a ramp, its end level. The senders
// (the shim's FfbForwarder, the virtual wheel's HidPid) know when each effect started and how long it lasts, so they
// work out the effect at every moment with shapeAt() and send that: the service and the wire format only ever see
// plain levels. Header only: the driver compiles HidPid.cpp without the rest of core.

namespace wheelbri {

// DIENVELOPE / HID PID Set Envelope. Levels 0..1 and absolute (the force keeps the effect's own sign); times in ms.
// From the start the level goes from attackLevel to the effect's own over attackMs, and over the last fadeMs of
// each play (only when the duration is known) from the effect's own to fadeLevel.
struct FfbEnvelope {
    float attackLevel = 0.0F;
    float attackMs = 0.0F;
    float fadeLevel = 0.0F;
    float fadeMs = 0.0F;

    friend bool operator==(const FfbEnvelope&, const FfbEnvelope&) = default;
};

struct FfbShape {
    std::optional<FfbEnvelope> envelope;
    // A ramp: the effect's constantLevel is its start, this its end (the same units, direction included). It goes
    // from one to the other over the duration; with no duration it plays at the mean level.
    std::optional<float> rampEndLevel;

    friend bool operator==(const FfbShape&, const FfbShape&) = default;
};

// A duration for an effect that plays until it is stopped (no fade, a ramp at its mean level).
inline constexpr double kUntilStopped = std::numeric_limits<double>::infinity();

// The level `sustain` (0..1) at `elapsedMs` into one play of `durationMs` (kUntilStopped: no fade).
// Where attack and fade overlap (a play shorter than both), the fade wins.
[[nodiscard]] inline float envelopeLevel(const float sustain, const FfbEnvelope& envelope, const double elapsedMs,
                                         const double durationMs) {
    const double t = std::max(0.0, elapsedMs);
    double level = sustain;
    if (envelope.attackMs > 0.0F && t < envelope.attackMs) {
        level = envelope.attackLevel + (sustain - envelope.attackLevel) * (t / envelope.attackMs);
    }
    if (std::isfinite(durationMs) && envelope.fadeMs > 0.0F) {
        const double fadeStart = durationMs - envelope.fadeMs;
        if (t > fadeStart) {
            const double share = std::clamp((t - fadeStart) / envelope.fadeMs, 0.0, 1.0);
            level = sustain + (envelope.fadeLevel - sustain) * share;
        }
    }
    return static_cast<float>(std::clamp(level, 0.0, 1.0));
}

// `effect` as it plays `elapsedMs` into one play (an iteration) of `durationMs`. Conditions (spring, damper, ...)
// have no envelope and come back unchanged.
[[nodiscard]] inline FfbEffect shapeAt(const FfbEffect& effect, const FfbShape& shape, const double elapsedMs,
                                       const double durationMs) {
    FfbEffect out = effect;
    if (effect.type == FfbEffectType::Constant) {
        if (shape.rampEndLevel) {
            const float start = effect.constantLevel;
            const float end = *shape.rampEndLevel;
            if (std::isfinite(durationMs) && durationMs > 0.0) {
                const double share = std::clamp(std::max(0.0, elapsedMs) / durationMs, 0.0, 1.0);
                out.constantLevel = static_cast<float>(start + (end - start) * share);
            } else {
                out.constantLevel = (start + end) / 2.0F;
            }
        }
        if (shape.envelope) {
            const float level = envelopeLevel(std::fabs(out.constantLevel), *shape.envelope, elapsedMs, durationMs);
            out.constantLevel = std::copysign(level, out.constantLevel);
        }
        return out;
    }
    // Not isConditionEffect (Ffb.cpp): the driver does not compile that file.
    const bool condition = effect.type == FfbEffectType::Spring || effect.type == FfbEffectType::Damper ||
                           effect.type == FfbEffectType::Friction || effect.type == FfbEffectType::Inertia;
    if (condition || !shape.envelope) {
        return out;
    }
    out.periodic.magnitude = envelopeLevel(effect.periodic.magnitude, *shape.envelope, elapsedMs, durationMs);
    return out;
}

}  // namespace wheelbri
