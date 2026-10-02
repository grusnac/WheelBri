#include "wheelemu/StatePacket.hpp"

namespace wheelemu {

namespace {

class ByteWriter {
public:
    explicit ByteWriter(std::span<std::uint8_t> out) : out_(out) {}

    void u16(const std::uint16_t value) {
        out_[offset_++] = static_cast<std::uint8_t>(value);
        out_[offset_++] = static_cast<std::uint8_t>(value >> 8);
    }

    void u32(const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out_[offset_++] = static_cast<std::uint8_t>(value >> shift);
        }
    }

    void i32(const std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void i8(const std::int8_t value) { out_[offset_++] = static_cast<std::uint8_t>(value); }

private:
    std::span<std::uint8_t> out_;
    std::size_t offset_ = 0;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> in) : in_(in) {}

    std::uint16_t u16() {
        const auto value = static_cast<std::uint16_t>(in_[offset_] | (in_[offset_ + 1] << 8));
        offset_ += 2;
        return value;
    }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(in_[offset_++]) << shift;
        }
        return value;
    }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int8_t i8() { return static_cast<std::int8_t>(in_[offset_++]); }

private:
    std::span<const std::uint8_t> in_;
    std::size_t offset_ = 0;
};

}  // namespace

std::array<std::uint8_t, StatePacket::kPacketSize> encode(const StatePacket& packet) {
    std::array<std::uint8_t, StatePacket::kPacketSize> bytes{};
    ByteWriter writer(bytes);
    writer.u32(StatePacket::kMagic);
    writer.u16(StatePacket::kVersion);
    writer.u16(0);
    writer.u32(packet.sequence);
    writer.i32(packet.report.steering);
    writer.i32(packet.report.throttle);
    writer.i32(packet.report.brake);
    writer.i32(packet.report.clutch);
    writer.u32(packet.report.buttons);
    writer.i8(packet.report.hat);
    return bytes;
}

std::optional<StatePacket> decodeStatePacket(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != StatePacket::kPacketSize) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.u32() != StatePacket::kMagic || reader.u16() != StatePacket::kVersion) {
        return std::nullopt;
    }
    reader.u16();  // reserved

    StatePacket packet;
    packet.sequence = reader.u32();
    packet.report.steering = reader.i32();
    packet.report.throttle = reader.i32();
    packet.report.brake = reader.i32();
    packet.report.clutch = reader.i32();
    packet.report.buttons = reader.u32();
    packet.report.hat = reader.i8();
    return packet;
}

}  // namespace wheelemu
