// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/MakerFfb.hpp"

#include "wheelbri/HidPid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace wheelbri {

namespace {

std::string hex(const std::span<const std::uint8_t> bytes) {
    std::string text;
    char b[4];
    for (const std::uint8_t v : bytes.first(std::min<std::size_t>(bytes.size(), 16))) {
        std::snprintf(b, sizeof(b), text.empty() ? "%02x" : " %02x", static_cast<unsigned>(v));
        text += b;
    }
    return text;
}

}  // namespace

MakerFfb::MakerFfb(Sender send, Logger log) : send_(std::move(send)), log_(std::move(log)) {}

float MakerFfb::level(const std::uint16_t force) {
    // 32768 steps up (0x8000..0xffff), 32767 down (0x7ffe..0x0000).
    const int above = static_cast<int>(force) - static_cast<int>(kNoForce);
    return std::clamp(static_cast<float>(above) / (above > 0 ? 32768.0F : 32767.0F), -1.0F, 1.0F);
}

void MakerFfb::note(const std::string& what) {
    if (!noted_.insert(what).second) return;
    if (log_) log_(LogLevel::Info, "maker ffb: " + what);
}

void MakerFfb::output(const std::span<const std::uint8_t> report, const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    if (report.empty()) return;
    if (report[0] != kForceReport || report.size() < 3) {
        char text[64];
        std::snprintf(text, sizeof(text), "report 0x%02x (%zu bytes): not played", static_cast<unsigned>(report[0]),
                      report.size());
        note(text);
        return;
    }
    if (reports_ == 0 || nowMs - lastReportMs_ > HidPid::kStreamGapMs) streamStartMs_ = nowMs;
    ++reports_;
    lastReportMs_ = nowMs;
    if (quiet_) {
        quiet_ = false;
        sentOnce_ = false;   // the service stopped the force: send it again
        if (log_) log_(LogLevel::Info, "maker ffb: the game sends again: keep-alives back");
    }
    const auto force = static_cast<std::uint16_t>((report[1] << 8) | report[2]);
    level_ = level(force);
    if (log_ && force != kNoForce) {
        const bool shown = shown_ < kShownReports;
        if (shown) ++shown_;
        char text[48];
        std::snprintf(text, sizeof(text), " -> %.3f", static_cast<double>(level_));
        log_(shown ? LogLevel::Info : LogLevel::Debug, "maker ffb: " + hex(report) + text);
    }
    forceSeen_ = true;
    if (!sentOnce_ || level_ != sentLevel_) send(FfbPacket::Kind::SlotState, true, level_, nowMs);
}

void MakerFfb::send(const FfbPacket::Kind kind, const bool playing, const float level, const std::int64_t nowMs) {
    FfbPacket packet;
    packet.kind = kind;
    packet.slot = 0;
    packet.sequence = ++sequence_;
    packet.playing = playing;
    packet.effect.type = FfbEffectType::Constant;
    packet.effect.constantLevel = level;
    if (send_) send_(packet);
    if (kind == FfbPacket::Kind::SlotState) {
        sentOnce_ = true;
        sentLevel_ = level;
    }
    lastSendMs_ = nowMs;
    ++packetsSent_;
}

void MakerFfb::tick(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    if (!forceSeen_) return;   // keep-alives only once a force came
    const bool pushing = std::fabs(sentLevel_) > HidPid::kStreamPushingAbove;
    const bool streamed = lastReportMs_ - streamStartMs_ >= HidPid::kStreamMinMs;
    if (!quiet_ && streamed && nowMs - lastReportMs_ >= HidPid::kStreamQuietMs && pushing) {
        quiet_ = true;
        if (log_) {
            log_(LogLevel::Info, "maker ffb: the game streamed force feedback and then stopped (closed, crashed or "
                                 "paused?): keep-alives stopped, so the service stops the forces");
        }
    }
    if (!quiet_ && nowMs - lastReportMs_ >= HidPid::kGameQuietMs && pushing) {
        quiet_ = true;
        if (log_) {
            log_(LogLevel::Warn, "maker ffb: nothing from the game for " +
                                     std::to_string(HidPid::kGameQuietMs / 1000) +
                                     " s while a force plays: keep-alives stopped");
        }
    }
    if (quiet_) return;
    if (nowMs - lastRefreshMs_ >= kRefreshMs) {
        lastRefreshMs_ = nowMs;
        send(FfbPacket::Kind::SlotState, true, level_, nowMs);
    }
    if (nowMs - lastSendMs_ >= kKeepAliveMs) send(FfbPacket::Kind::KeepAlive, false, 0.0F, nowMs);
}

void MakerFfb::stopAll(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    sentOnce_ = false;
    sentLevel_ = 0.0F;
    level_ = 0.0F;
    if (forceSeen_) send(FfbPacket::Kind::StopAll, false, 0.0F, nowMs);
}

std::uint64_t MakerFfb::packetsSent() const {
    std::lock_guard lock(mutex_);
    return packetsSent_;
}

}  // namespace wheelbri
