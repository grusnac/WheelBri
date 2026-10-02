#include "wheelemu/StatePacket.hpp"

#include <gtest/gtest.h>

namespace wheelemu {
namespace {

StatePacket samplePacket() {
    StatePacket packet;
    packet.sequence = 0xDEADBEEF;
    packet.report = {-12345, 65535, 0, 32768, 0x80000001U, 7};
    return packet;
}

TEST(StatePacket, RoundTrips) {
    const StatePacket original = samplePacket();
    const auto bytes = encode(original);
    const auto decoded = decodeStatePacket(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->sequence, original.sequence);
    EXPECT_EQ(decoded->report, original.report);
}

TEST(StatePacket, LayoutIsLittleEndianWithMagicFirst) {
    const auto bytes = encode(samplePacket());
    EXPECT_EQ(bytes[0], 'W');
    EXPECT_EQ(bytes[1], 'E');
    EXPECT_EQ(bytes[2], 'M');
    EXPECT_EQ(bytes[3], 'U');
    EXPECT_EQ(bytes[4], StatePacket::kVersion);
    EXPECT_EQ(bytes[8], 0xEF);                  // sequence, low byte first
    EXPECT_EQ(bytes[32], 7);                    // hat
}

TEST(StatePacket, RejectsWrongSizeMagicOrVersion) {
    auto bytes = encode(samplePacket());
    EXPECT_FALSE(decodeStatePacket(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1)).has_value());

    auto badMagic = bytes;
    badMagic[0] = 'X';
    EXPECT_FALSE(decodeStatePacket(badMagic).has_value());

    auto badVersion = bytes;
    badVersion[4] = StatePacket::kVersion + 1;
    EXPECT_FALSE(decodeStatePacket(badVersion).has_value());
}

TEST(StatePacket, CenteredHatSurvives) {
    StatePacket packet = samplePacket();
    packet.report.hat = kHatCentered;
    const auto decoded = decodeStatePacket(encode(packet));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->report.hat, kHatCentered);
}

}  // namespace
}  // namespace wheelemu
