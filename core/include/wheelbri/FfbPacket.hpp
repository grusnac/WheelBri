#pragma once

#include "wheelbri/Ffb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace wheelbri {

// Wire format for force feedback, from the in-game shim to the service (UDP loopback, the reverse
// direction of StatePacket). The shim sends the full state of one effect slot whenever it changes,
// and a keep-alive at least every 100 ms, so a lost datagram heals itself and the service can stop
// every force when the game goes quiet (crash, pause, alt-tab).
//
// Layout (little-endian, 64 bytes):
//   0  u32 magic 'WEFB'   4  u16 version   6  u8 kind   7  u8 slot   8  u32 sequence
//  12  u8 playing  13 u8 effect type  14 u16 reserved
//  16  f32 gain  20 f32 constant level
//  24  f32 condition: center, deadband, +coefficient, -coefficient, +saturation, -saturation (24 bytes)
//  48  f32 periodic: magnitude, offset, period ms, phase deg (16 bytes)
struct FfbPacket {
    enum class Kind : std::uint8_t {
        SlotState = 0,   // `slot` now plays (or not) `effect`
        KeepAlive = 1,   // nothing changed; the game is still there
        StopAll = 2,     // game reset its force feedback (new device, DISFFC_RESET / STOPALL)
    };

    static constexpr std::uint32_t kMagic = 0x42464557;  // "WEFB" little-endian
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kPacketSize = 64;

    Kind kind = Kind::KeepAlive;
    std::uint8_t slot = 0;
    std::uint32_t sequence = 0;
    bool playing = false;
    FfbEffect effect;
};

[[nodiscard]] std::array<std::uint8_t, FfbPacket::kPacketSize> encode(const FfbPacket& packet);

// Returns nullopt for anything that is not a well-formed packet of the current version.
[[nodiscard]] std::optional<FfbPacket> decodeFfbPacket(std::span<const std::uint8_t> bytes);

}  // namespace wheelbri
