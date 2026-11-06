// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the decoder knows of the two protocols, as WheelBri's TrueForce.hpp and HidPp.hpp describe them, and which
// device is the one being captured.

#include "g923cap/Pcap.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace wheelbri::g923cap {

inline constexpr std::uint16_t kLogitech = 0x046D;
inline constexpr std::uint16_t kG923 = 0xC26E;   // G923 for Xbox One and PC, PC mode

// Other G923 product IDs (for the notes; kept with the wheel when they show up in a capture).
[[nodiscard]] const char* otherG923Name(std::uint16_t product);

// G923_TARGET=VID[:PID] (hex, e.g. 346E for the MOZA R3): a rehearsal with another wheel. None = the G923.
struct Target {
    std::uint16_t vendor = 0;
    std::optional<std::uint16_t> product;
};
[[nodiscard]] std::optional<Target> rehearsalTarget();
// Whether a device descriptor is the wheel being captured (the G923 in any mode, or the rehearsal wheel).
[[nodiscard]] bool isTarget(std::uint16_t vendor, std::uint16_t product);

// --- TrueForce: report 0x01, 64 bytes: [01 00 00 00, type, sequence, payload...] ------------------------------------

[[nodiscard]] bool isTrueForceReport(std::span<const std::uint8_t> d);
// The bytes from [4] up to and including `at` sum to 0 (mod 256).
[[nodiscard]] bool sealedAt(std::span<const std::uint8_t> d, std::size_t at);
// Where a known answer type carries its checksum; none for others.
[[nodiscard]] std::optional<std::size_t> sealOf(std::uint8_t type);
// An answer of unknown type: its last non-zero byte, when the bytes up to it sum to 0.
[[nodiscard]] std::optional<std::size_t> findSeal(std::span<const std::uint8_t> d);
[[nodiscard]] bool isStatusType(std::uint8_t type);
// The answer types a request type gets (empty: unknown request).
[[nodiscard]] std::span<const std::uint8_t> expectedAnswers(std::uint8_t requestType);
[[nodiscard]] std::string typeName(std::uint8_t type);

[[nodiscard]] std::uint32_t u24At(std::span<const std::uint8_t> d, std::size_t at);
[[nodiscard]] std::uint32_t u32At(std::span<const std::uint8_t> d, std::size_t at);

inline constexpr double kTorqueUnit = 65536000.0;   // full scale: int32 + 0x80000000, Nm * 65536000

// --- HID++: [report ID, device index, feature index, function << 4 | software id, parameters...] -------------------

[[nodiscard]] bool isHidpp(std::span<const std::uint8_t> d);   // 0x10/0x11/0x12, 7/20/64 bytes
[[nodiscard]] bool isHidppError(std::span<const std::uint8_t> answer);
[[nodiscard]] bool hidppAnswers(std::span<const std::uint8_t> request, std::span<const std::uint8_t> answer);
[[nodiscard]] std::string featureName(std::optional<std::uint16_t> id);
[[nodiscard]] std::string hidppErrorName(std::uint8_t code);

// --- Text --------------------------------------------------------------------------------------------------------

// "01 00 00 00 …" with at most `n` bytes, then "… (+k bytes, all zero)".
[[nodiscard]] std::string hexOf(std::span<const std::uint8_t> d, std::size_t n = 40);
[[nodiscard]] std::string hexPlain(std::span<const std::uint8_t> d);   // "0100ff"
[[nodiscard]] std::string asciiOf(std::span<const std::uint8_t> d);
// %.6g, as the summaries print numbers.
[[nodiscard]] std::string numberText(double v);

}  // namespace wheelbri::g923cap
