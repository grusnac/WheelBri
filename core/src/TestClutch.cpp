#include "wheelbri/TestClutch.hpp"

#include <algorithm>
#include <cmath>

namespace wheelbri {

namespace {

std::uint32_t readU32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16U) | (static_cast<std::uint32_t>(b[at + 3]) << 24U);
}

std::uint16_t readU16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8U));
}

}  // namespace

std::optional<float> ClutchPacket::decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kSize || readU32(bytes, 0) != kMagic || readU16(bytes, 4) != kVersion) {
        return std::nullopt;
    }
    return static_cast<float>(std::min<std::uint16_t>(readU16(bytes, 8), 10000)) / 10000.0F;
}

std::array<std::uint8_t, ClutchPacket::kSize> ClutchPacket::encode(float pressed) {
    std::array<std::uint8_t, kSize> b{};
    const auto put16 = [&b](std::size_t at, std::uint16_t v) {
        b[at] = static_cast<std::uint8_t>(v & 0xFFU);
        b[at + 1] = static_cast<std::uint8_t>(v >> 8U);
    };
    put16(0, static_cast<std::uint16_t>(kMagic & 0xFFFFU));
    put16(2, static_cast<std::uint16_t>(kMagic >> 16U));
    put16(4, kVersion);
    put16(8, static_cast<std::uint16_t>(std::lround(std::clamp(pressed, 0.0F, 1.0F) * 10000.0F)));
    return b;
}

void TestClutch::offer(float pressed, std::uint64_t nowNs) {
    pressed_ = std::clamp(pressed, 0.0F, 1.0F);
    lastNs_ = nowNs;
}

std::optional<float> TestClutch::active(std::uint64_t nowNs) const {
    if (!lastNs_ || nowNs - *lastNs_ > kHoldNs) {
        return std::nullopt;
    }
    return pressed_;
}

}  // namespace wheelbri
