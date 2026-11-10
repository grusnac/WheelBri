// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/StatePacket.hpp"

#include "wheelbri/ByteOrder.hpp"

#include <algorithm>

namespace wheelbri {

namespace {

using ByteWriter = bytes::Writer;
using ByteReader = bytes::Reader;

}  // namespace

std::array<std::uint8_t, StatePacket::kPacketSize> encode(const StatePacket& packet) {
    std::array<std::uint8_t, StatePacket::kPacketSize> bytes{};
    ByteWriter writer(bytes);
    writer.u32(StatePacket::kMagic);
    writer.u16(StatePacket::kVersion);
    writer.u16(packet.rotationDeg);
    writer.u32(packet.sequence);
    writer.i32(packet.report.steering);
    writer.i32(packet.report.throttle);
    writer.i32(packet.report.brake);
    writer.i32(packet.report.clutch);
    writer.u32(packet.report.buttons);
    writer.i8(packet.report.hat);
    writer.i32(packet.report.combined);
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
    const std::uint16_t rotationDeg = reader.u16();

    StatePacket packet;
    packet.rotationDeg = rotationDeg;
    packet.sequence = reader.u32();
    // Only values an emulated wheel can report (0..65535, hat -1..7): whatever sent this, the game gets sane input.
    const auto axis = [&reader] { return std::clamp<std::int32_t>(reader.i32(), 0, 65535); };
    packet.report.steering = axis();
    packet.report.throttle = axis();
    packet.report.brake = axis();
    packet.report.clutch = axis();
    packet.report.buttons = reader.u32();
    const std::int8_t hat = reader.i8();
    packet.report.hat = hat >= 0 && hat <= 7 ? hat : kHatCentered;
    packet.report.combined = axis();
    return packet;
}

}  // namespace wheelbri
