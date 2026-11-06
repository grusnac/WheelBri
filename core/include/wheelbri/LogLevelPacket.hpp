// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/LogLevel.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace wheelbri {

// The log level the app picked (its Level button), sent by the service to whatever reads the wheel's state on
// state_port (41329): the shim in the game and wheelbri-uhid take it (the virtual wheel's driver ignores it: it reads
// the registry, HKLM\SOFTWARE\wheelbri LogLevel). It goes out from the state's own socket when a session starts, at
// once when the level changes, and every kRepeatMs (a game started later). A reader that does not know it drops it
// (an older shim logs it as a malformed datagram, at most once every 5 s).
//
// Layout (little-endian, 8 bytes): 0 u32 magic 'WELV'   4 u8 version   5 u8 level (LogLevel)   6 u16 reserved 0.
// Header only: the driver compiles it without the rest of core.
struct LogLevelPacket {
    static constexpr std::uint32_t kMagic = 0x564C4557;   // "WELV" little-endian
    static constexpr std::uint8_t kVersion = 1;
    static constexpr std::size_t kPacketSize = 8;
    static constexpr std::uint64_t kRepeatMs = 2'000;

    LogLevel level = kDefaultAppLogLevel;
};

[[nodiscard]] constexpr std::array<std::uint8_t, LogLevelPacket::kPacketSize> encode(const LogLevelPacket& packet) {
    return {static_cast<std::uint8_t>(LogLevelPacket::kMagic & 0xFFU),
            static_cast<std::uint8_t>((LogLevelPacket::kMagic >> 8) & 0xFFU),
            static_cast<std::uint8_t>((LogLevelPacket::kMagic >> 16) & 0xFFU),
            static_cast<std::uint8_t>((LogLevelPacket::kMagic >> 24) & 0xFFU),
            LogLevelPacket::kVersion,
            static_cast<std::uint8_t>(packet.level),
            0,
            0};
}

// Nullopt for anything but a well-formed packet of this version. The level is clamped the way the app clamps it:
// warnings and errors are always written whatever the packet says.
[[nodiscard]] constexpr std::optional<LogLevelPacket> decodeLogLevelPacket(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != LogLevelPacket::kPacketSize) return std::nullopt;
    const std::uint32_t magic = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                (static_cast<std::uint32_t>(bytes[3]) << 24);
    if (magic != LogLevelPacket::kMagic || bytes[4] != LogLevelPacket::kVersion) return std::nullopt;
    if (bytes[5] > static_cast<std::uint8_t>(LogLevel::Off) || bytes[6] != 0 || bytes[7] != 0) return std::nullopt;
    return LogLevelPacket{clampToApp(static_cast<LogLevel>(bytes[5]))};
}

// When the session sends its LogLevelPacket: the first time, at once on a new level, and kRepeatMs after the last.
// Allocation-free; asked on every pass of the service loop.
class LogLevelBeacon {
public:
    [[nodiscard]] constexpr bool due(const std::uint64_t nowMs, const LogLevel level) {
        if (sent_ && level == level_ && nowMs >= lastMs_ && nowMs - lastMs_ < LogLevelPacket::kRepeatMs) return false;
        sent_ = true;
        level_ = level;
        lastMs_ = nowMs;
        return true;
    }

private:
    bool sent_ = false;
    LogLevel level_ = kDefaultAppLogLevel;
    std::uint64_t lastMs_ = 0;
};

}  // namespace wheelbri
