#include "wheelbri/FfbPacket.hpp"

#include "wheelbri/ByteOrder.hpp"

#include <bit>
#include <cmath>

namespace wheelbri {

namespace {

using Writer = bytes::Writer;

// Non-finite values (never sent on purpose) become 0 so they cannot reach the wheel.
class Reader : public bytes::Reader {
public:
    using bytes::Reader::Reader;
    float f32() {
        const float value = bytes::Reader::f32();
        return std::isfinite(value) ? value : 0.0F;
    }
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
