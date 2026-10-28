#include "wheelbri/TrueForceFfb.hpp"

#include "wheelbri/TrueForce.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace wheelbri {

TrueForceFfb::TrueForceFfb(Sink sink) : sink_(std::move(sink)) {}

std::optional<float> TrueForceFfb::level(const std::span<const std::uint8_t> report) {
    if (report.size() < 10 || report[0] != kTrueForceReportId || report[4] != 0x01) return std::nullopt;
    const int cur = report[6] | (report[7] << 8);
    return std::clamp(static_cast<float>(0x8000 - cur) / 32768.0F, -1.0F, 1.0F);
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
    } else if (damperPlaying_) {
        sendDamper(false, nowUs);
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

}  // namespace wheelbri
