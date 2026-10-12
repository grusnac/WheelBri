#include "wheelbri/FfbPacket.hpp"

#include <bit>
#include <cmath>

namespace wheelbri {

namespace {

class Writer {
public:
    explicit Writer(std::span<std::uint8_t> out) : out_(out) {}

    void u8(const std::uint8_t value) { out_[offset_++] = value; }
    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void f32(const float value) { u32(std::bit_cast<std::uint32_t>(value)); }

private:
    std::span<std::uint8_t> out_;
    std::size_t offset_ = 0;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> in) : in_(in) {}

    std::uint8_t u8() { return in_[offset_++]; }
    std::uint16_t u16() {
        const std::uint16_t low = u8();
        return static_cast<std::uint16_t>(low | (u8() << 8));
    }
    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return value;
    }
    // Non-finite values (never sent on purpose) become 0 so they cannot reach the wheel.
    float f32() {
        const float value = std::bit_cast<float>(u32());
        return std::isfinite(value) ? value : 0.0F;
    }

private:
    std::span<const std::uint8_t> in_;
    std::size_t offset_ = 0;
};

constexpr std::uint8_t kLastEffectType = static_cast<std::uint8_t>(FfbEffectType::SawtoothDown);
constexpr std::uint8_t kLastKind = static_cast<std::uint8_t>(FfbPacket::Kind::StopAll);

}  // namespace

std::array<std::uint8_t, FfbPacket::kPacketSize> encode(const FfbPacket& packet) {
    std::array<std::uint8_t, FfbPacket::kPacketSize> bytes{};
    Writer writer(bytes);
    writer.u32(FfbPacket::kMagic);
    writer.u16(FfbPacket::kVersion);
    writer.u8(static_cast<std::uint8_t>(packet.kind));
    writer.u8(packet.slot);
    writer.u32(packet.sequence);
    writer.u8(packet.playing ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(packet.effect.type));
    writer.u16(0);

    const FfbEffect& e = packet.effect;
    writer.f32(e.gain);
    writer.f32(e.constantLevel);
    writer.f32(e.condition.center);
    writer.f32(e.condition.deadband);
    writer.f32(e.condition.positiveCoefficient);
    writer.f32(e.condition.negativeCoefficient);
    writer.f32(e.condition.positiveSaturation);
    writer.f32(e.condition.negativeSaturation);
    writer.f32(e.periodic.magnitude);
    writer.f32(e.periodic.offset);
    writer.f32(e.periodic.periodMs);
    writer.f32(e.periodic.phaseDeg);
    return bytes;
}

std::optional<FfbPacket> decodeFfbPacket(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != FfbPacket::kPacketSize) {
        return std::nullopt;
    }
    Reader reader(bytes);
    if (reader.u32() != FfbPacket::kMagic || reader.u16() != FfbPacket::kVersion) {
        return std::nullopt;
    }
    FfbPacket packet;
    const std::uint8_t kind = reader.u8();
    if (kind > kLastKind) {
        return std::nullopt;
    }
    packet.kind = static_cast<FfbPacket::Kind>(kind);
    packet.slot = reader.u8();
    packet.sequence = reader.u32();
    packet.playing = reader.u8() != 0;
    const std::uint8_t type = reader.u8();
    if (type > kLastEffectType) {
        return std::nullopt;
    }
    packet.effect.type = static_cast<FfbEffectType>(type);
    reader.u16();

    FfbEffect& e = packet.effect;
    e.gain = reader.f32();
    e.constantLevel = reader.f32();
    e.condition.center = reader.f32();
    e.condition.deadband = reader.f32();
    e.condition.positiveCoefficient = reader.f32();
    e.condition.negativeCoefficient = reader.f32();
    e.condition.positiveSaturation = reader.f32();
    e.condition.negativeSaturation = reader.f32();
    e.periodic.magnitude = reader.f32();
    e.periodic.offset = reader.f32();
    e.periodic.periodMs = reader.f32();
    e.periodic.phaseDeg = reader.f32();
    return packet;
}

}  // namespace wheelbri
