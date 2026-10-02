#pragma once

#include "wheelemu/DeviceProfile.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace wheelemu {

// Wire format from the service to the in-game shim, sent over UDP loopback.
// UDP rather than shared memory because under Proton the shim is a Windows DLL inside Wine
// and the service is a native Linux process: named mappings do not cross that boundary, sockets do.
//
// Layout (little-endian, 33 bytes):
//   0  u32 magic 'WEMU'   4  u16 version   6  u16 reserved   8  u32 sequence
//  12  i32 steering      16  i32 throttle  20  i32 brake     24  i32 clutch
//  28  u32 buttons       32  i8  hat
struct StatePacket {
    static constexpr std::uint32_t kMagic = 0x554D4557;  // "WEMU" little-endian
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kPacketSize = 33;

    std::uint32_t sequence = 0;
    TargetReport report;
};

[[nodiscard]] std::array<std::uint8_t, StatePacket::kPacketSize> encode(const StatePacket& packet);

// Returns nullopt for anything that is not a well-formed packet of the current version.
[[nodiscard]] std::optional<StatePacket> decodeStatePacket(std::span<const std::uint8_t> bytes);

}  // namespace wheelemu
