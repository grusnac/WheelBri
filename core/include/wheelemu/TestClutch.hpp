#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace wheelemu {

// A test clutch for when no clutch pedal is connected: tools/vbridge/ClutchProbe sends the position over
// UDP 127.0.0.1:41331 and the service uses it instead of the mapped clutch, so the UI, the telemetry and
// every game path (shim or virtual device) see it.
inline constexpr std::uint16_t kTestClutchPort = 41331;

// 12 bytes, little-endian: 0 u32 magic "WECL", 4 u16 version, 6 u16 reserved,
// 8 u16 pressed 0..10000 (0 = released), 10 u16 reserved. Same layout as ClutchPacket in tools/vbridge/Wire.cs.
struct ClutchPacket {
    static constexpr std::uint32_t kMagic = 0x4C434557;   // "WECL"
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kSize = 12;

    // Pressed share 0..1, or nullopt if the bytes are not a clutch packet.
    [[nodiscard]] static std::optional<float> decode(std::span<const std::uint8_t> bytes);
    [[nodiscard]] static std::array<std::uint8_t, kSize> encode(float pressed);
};

// Holds the last test-clutch position; it counts for kHoldNs after the last packet, then the pedal's clutch
// is used again.
class TestClutch {
public:
    static constexpr std::uint64_t kHoldNs = 1'500'000'000;

    void offer(float pressed, std::uint64_t nowNs);
    [[nodiscard]] std::optional<float> active(std::uint64_t nowNs) const;

private:
    float pressed_ = 0.0F;
    std::optional<std::uint64_t> lastNs_;
};

}  // namespace wheelemu
