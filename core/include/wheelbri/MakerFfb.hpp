// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/LogLevel.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <span>
#include <string>

namespace wheelbri {

// Thrustmaster's force feedback, played: what Thrustmaster's driver (tmffbdrv.dll, 2018.FFD.2) writes to the virtual
// RGT's maker identity (VirtualWheel::makerIdentity), turned into one constant force the service plays on the rig.
// The driver works the effects out itself and streams only the result, every 15-30 ms (GRID, 2026-11-24):
//   report 0x40, 5 bytes: [1..2] the force, 16 bits, high byte first; 0x7fff none; [3..4] always 00 00.
// Direction: above 0x7fff comes with the wheel right of centre (2176 of 2612 samples, wheelbri-driver-2026-11-24-01.log
// against its telemetry), and a game's centring force through Windows' HID PID driver has the sign of the wheel's
// position (GRID and others, G25 and RGT, 2026-11-22..23). So above 0x7fff is +, 0xffff +1, 0x0000 -1. (Played the
// other way, 2026-11-24, it pushed a Moza R3 away from the centre, lock to lock.)
// Other reports are logged once. Keep-alives and quiet rules as ClassicFfb's. Thread-safe.
class MakerFfb {
public:
    using Sender = std::function<void(const FfbPacket&)>;
    using Logger = std::function<void(LogLevel, const std::string&)>;
    static constexpr std::uint8_t kForceReport = 0x40;
    static constexpr std::uint16_t kNoForce = 0x7FFF;
    static constexpr std::int64_t kKeepAliveMs = 100;
    static constexpr std::int64_t kRefreshMs = 250;
    static constexpr int kShownReports = 30;   // at info, the rest at debug

    MakerFfb(Sender send, Logger log);

    // One output report, its ID first.
    void output(std::span<const std::uint8_t> report, std::int64_t nowMs);
    // Every ~10 ms: refreshes, keep-alives, quiet rules.
    void tick(std::int64_t nowMs);
    // Everything stops (device going away).
    void stopAll(std::int64_t nowMs);
    [[nodiscard]] std::uint64_t packetsSent() const;

    // A force as a signed level: 0x7fff -> 0, 0xffff -> +1, 0x0000 -> -1.
    [[nodiscard]] static float level(std::uint16_t force);

private:
    void send(FfbPacket::Kind kind, bool playing, float level, std::int64_t nowMs);
    void note(const std::string& what);

    Sender send_;
    Logger log_;
    mutable std::mutex mutex_;
    std::set<std::string> noted_;
    std::uint32_t sequence_ = 0;
    std::uint64_t packetsSent_ = 0;
    std::uint64_t reports_ = 0;
    int shown_ = 0;
    bool forceSeen_ = false;
    bool sentOnce_ = false;
    bool quiet_ = false;
    float level_ = 0.0F;
    float sentLevel_ = 0.0F;
    std::int64_t lastReportMs_ = 0;
    std::int64_t streamStartMs_ = 0;
    std::int64_t lastSendMs_ = 0;
    std::int64_t lastRefreshMs_ = 0;
};

}  // namespace wheelbri
