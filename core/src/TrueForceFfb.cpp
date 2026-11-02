#include "wheelbri/TrueForceFfb.hpp"

#include "wheelbri/TrueForce.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace wheelbri {

TrueForceFfb::TrueForceFfb(Sink sink) : sink_(std::move(sink)) {}

std::optional<float> TrueForceFfb::level(const std::span<const std::uint8_t> report) {
    if (report.size() < 10 || report[0] != kTrueForceReportId || report[4] != 0x01) return std::nullopt;
    const int cur = streamTarget(report);
    return std::clamp(static_cast<float>(0x8000 - cur) / 32768.0F, -1.0F, 1.0F);
}

std::size_t TrueForceFfb::texture(const std::span<const std::uint8_t> report, const std::span<float> out) {
    constexpr std::size_t kSlots = 13;
    constexpr std::size_t kFirst = 12;
    if (report.size() < kFirst + kSlots * 4 || report[0] != kTrueForceReportId || report[4] != 0x01 ||
        report[11] != 0x0D) {
        return 0;
    }
    const std::size_t fresh = std::min<std::size_t>({report[10], kSlots, out.size()});
    for (std::size_t i = 0; i < fresh; ++i) {
        const std::size_t at = kFirst + (kSlots - fresh + i) * 4;
        const int left = report[at] | (report[at + 1] << 8);
        const int right = report[at + 2] | (report[at + 3] << 8);
        out[i] = std::clamp(static_cast<float>(0x8000 - (left + right) / 2.0F) / 32768.0F, -1.0F, 1.0F);
    }
    return fresh;
}

void TrueForceFfb::output(const std::span<const std::uint8_t> report, const std::int64_t nowUs) {
    const std::optional<float> next = level(report);
    std::lock_guard guard(lock_);
    if (!next) {
        const bool ours = report.size() > 4 && report[0] == kTrueForceReportId;
        // A stop: no force, but the slots keep playing, at 0; they go quiet only when no stream follows (tick). The
        // SDK stops and starts again within milliseconds whenever a game re-opens the wheel, and stopping and
        // restarting the rig's effects for that moved a Moza R3 (2026-10-28).
        if (ours && report[4] == 0x04 && playing_) {
            level_ = 0.0F;
            if (sentLevel_ != 0.0F) send(true, nowUs);
            resetTexture();
            if (texturePlaying_) sendTexture(false, nowUs);
        }
        if (ours && report[4] == 0x0C && report.size() >= 10) {
            float damping = 0.0F;
            std::memcpy(&damping, report.data() + 6, sizeof(damping));
            damping_ = std::isfinite(damping) ? std::clamp(damping / kDampingMax, 0.0F, 1.0F) : 0.0F;
            if (playing_) updateDamper(nowUs);
        }
        return;
    }
    lastStreamUs_ = nowUs;
    level_ = *next;
    if (!playing_ || (level_ != sentLevel_ && nowUs - lastSentUs_ >= kMinGapUs)) {
        send(true, nowUs);
    } else if (level_ != sentLevel_) {
        pending_ = true;
    }
    std::array<float, 13> samples{};
    const std::size_t fresh = texture(report, samples);
    if (fresh > 0) addTexture(std::span<const float>(samples.data(), fresh), nowUs);
}

void TrueForceFfb::tick(const std::int64_t nowUs) {
    std::lock_guard guard(lock_);
    if (!playing_) return;
    if (nowUs - lastStreamUs_ >= kQuietUs) {
        send(false, nowUs);
    } else if ((pending_ && nowUs - lastSentUs_ >= kMinGapUs) || nowUs - lastSentUs_ >= kKeepAliveUs) {
        send(true, nowUs);
    }
}

void TrueForceFfb::stop(const std::int64_t nowUs) {
    std::lock_guard guard(lock_);
    if (playing_) send(false, nowUs);
}

bool TrueForceFfb::playing() const {
    std::lock_guard guard(lock_);
    return playing_;
}

void TrueForceFfb::send(const bool playing, const std::int64_t nowUs) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = kSlot;
    packet.sequence = ++sequence_;
    packet.playing = playing;
    packet.effect.type = FfbEffectType::Constant;
    packet.effect.constantLevel = playing ? level_ : 0.0F;
    packet.effect.gain = 1.0F;
    playing_ = playing;
    sentLevel_ = level_;
    pending_ = false;
    lastSentUs_ = nowUs;
    if (sink_) sink_(packet);
    if (playing) {
        updateDamper(nowUs);
        updateTexture(nowUs);
    } else {
        if (damperPlaying_) sendDamper(false, nowUs);
        resetTexture();
        if (texturePlaying_) sendTexture(false, nowUs);
    }
}

void TrueForceFfb::updateDamper(const std::int64_t nowUs) {
    if (damping_ <= 0.0F) {
        if (damperPlaying_) sendDamper(false, nowUs);
    } else if (!damperPlaying_ || damping_ != sentDamping_ || nowUs - lastDamperUs_ >= kKeepAliveUs) {
        sendDamper(true, nowUs);
    }
}

void TrueForceFfb::sendDamper(const bool playing, const std::int64_t nowUs) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = kDamperSlot;
    packet.sequence = ++sequence_;
    packet.playing = playing;
    packet.effect.type = FfbEffectType::Damper;
    packet.effect.condition.positiveCoefficient = playing ? damping_ : 0.0F;
    packet.effect.condition.negativeCoefficient = packet.effect.condition.positiveCoefficient;
    packet.effect.gain = 1.0F;
    damperPlaying_ = playing;
    sentDamping_ = damping_;
    lastDamperUs_ = nowUs;
    if (sink_) sink_(packet);
}

void TrueForceFfb::addTexture(const std::span<const float> samples, const std::int64_t nowUs) {
    // A zero crossing past kTextureHysteresis: counted, with the first's and last's place in the window.
    const auto cross = [this](Crossings& c, const float value) {
        const int side = value > kTextureHysteresis ? 1 : value < -kTextureHysteresis ? -1 : 0;
        if (side == 0) return;
        if (c.side != 0 && side != c.side) {
            if (c.count == 0) c.first = windowCount_;
            c.last = windowCount_;
            ++c.count;
        }
        c.side = side;
    };
    for (const float x : samples) {
        windowSum_ += x;
        windowSumSq_ += static_cast<double>(x) * x;
        // Crossings of the running mean of the last kTextureSmoothing samples: the raw samples cross far more often
        // than the vibration they carry does (ACC: 390 Hz raw, 250 Hz smoothed, against 240 Hz for the strongest
        // frequency of each window). The raw ones stand in when the mean has too few (a texture at 1 kHz, which it
        // cancels out).
        smoothSum_ += x - smooth_.at(smoothNext_);
        smooth_.at(smoothNext_) = x;
        smoothNext_ = (smoothNext_ + 1) % smooth_.size();
        cross(smoothCrossings_, smoothSum_ / static_cast<float>(smooth_.size()));
        cross(rawCrossings_, x);
        if (++windowCount_ < kTextureSamples) continue;
        const double n = static_cast<double>(windowCount_);
        const double mean = windowSum_ / n;
        const double variance = std::max(0.0, windowSumSq_ / n - mean * mean);
        const auto magnitude = static_cast<float>(std::sqrt(2.0 * variance));
        textureMagnitude_ = magnitude < kTextureQuiet ? 0.0F : std::min(magnitude, kTextureMax);
        // Half a period between neighbouring crossings: the span from the first to the last over the halves in it.
        const Crossings& c = smoothCrossings_.count >= 2 ? smoothCrossings_ : rawCrossings_;
        float periodMs = kTextureMaxPeriodMs;
        if (c.count >= 2) {
            const double halves = static_cast<double>(c.count - 1);
            periodMs = static_cast<float>(2.0 * static_cast<double>(c.last - c.first) / halves /
                                          kTextureSampleRateHz * 1000.0);
        }
        texturePeriodMs_ = std::clamp(periodMs, kTextureMinPeriodMs, kTextureMaxPeriodMs);
        startWindow();
        if (playing_) updateTexture(nowUs);
    }
}

void TrueForceFfb::startWindow() {
    windowCount_ = 0;
    windowSum_ = 0;
    windowSumSq_ = 0;
    smoothCrossings_.count = 0;
    rawCrossings_.count = 0;
}

void TrueForceFfb::resetTexture() {
    startWindow();
    smoothCrossings_.side = 0;
    rawCrossings_.side = 0;
    smooth_.fill(0.0F);
    smoothNext_ = 0;
    smoothSum_ = 0.0F;
    textureMagnitude_ = 0.0F;
}

void TrueForceFfb::updateTexture(const std::int64_t nowUs) {
    if (textureMagnitude_ <= 0.0F) {
        if (texturePlaying_) sendTexture(false, nowUs);
        return;
    }
    const bool magnitudeMoved =
        std::abs(textureMagnitude_ - sentTextureMagnitude_) > kTextureStep * sentTextureMagnitude_;
    const bool periodMoved =
        std::abs(texturePeriodMs_ - sentTexturePeriodMs_) > kTexturePeriodStep * sentTexturePeriodMs_;
    if (!texturePlaying_ || magnitudeMoved || periodMoved || nowUs - lastTextureUs_ >= kKeepAliveUs) {
        sendTexture(true, nowUs);
    }
}

void TrueForceFfb::sendTexture(const bool playing, const std::int64_t nowUs) {
    FfbPacket packet;
    packet.kind = FfbPacket::Kind::SlotState;
    packet.slot = kTextureSlot;
    packet.sequence = ++sequence_;
    packet.playing = playing;
    packet.effect.type = FfbEffectType::Sine;
    packet.effect.periodic.magnitude = playing ? textureMagnitude_ : 0.0F;
    packet.effect.periodic.periodMs = playing ? texturePeriodMs_ : sentTexturePeriodMs_;
    packet.effect.gain = 1.0F;
    texturePlaying_ = playing;
    sentTextureMagnitude_ = packet.effect.periodic.magnitude;
    if (playing) sentTexturePeriodMs_ = texturePeriodMs_;
    lastTextureUs_ = nowUs;
    if (sink_) sink_(packet);
}

}  // namespace wheelbri
