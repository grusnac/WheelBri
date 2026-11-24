// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/StatePacket.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

StatePacket samplePacket() {
    StatePacket packet;
    packet.sequence = 0xDEADBEEF;
    packet.rotationDeg = 540;
    packet.report = {12345, 65535, 0, 32768, 0x80000001U, 7, 0x0000ABCD};
    return packet;
}

TEST(StatePacket, RoundTrips) {
    const StatePacket original = samplePacket();
    const auto bytes = encode(original);
    const auto decoded = decodeStatePacket(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->sequence, original.sequence);
    EXPECT_EQ(decoded->rotationDeg, 540);
    EXPECT_EQ(decoded->report, original.report);
}

TEST(StatePacket, OutOfRangeValuesAreClampedOnDecode) {
    StatePacket packet = samplePacket();
    packet.report.steering = -5;
    packet.report.throttle = 1'000'000;
    packet.report.brake = INT32_MIN;
    packet.report.hat = 42;
    const auto decoded = decodeStatePacket(encode(packet));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->report.steering, 0);
    EXPECT_EQ(decoded->report.throttle, 65535);
    EXPECT_EQ(decoded->report.brake, 0);
    EXPECT_EQ(decoded->report.hat, kHatCentered);
}

TEST(StatePacket, LayoutIsLittleEndianWithMagicFirst) {
    const auto bytes = encode(samplePacket());
    EXPECT_EQ(bytes[0], 'W');
    EXPECT_EQ(bytes[1], 'E');
    EXPECT_EQ(bytes[2], 'M');
    EXPECT_EQ(bytes[3], 'U');
    EXPECT_EQ(bytes[4], StatePacket::kVersion);
    EXPECT_EQ(bytes[6], 0x1C);                  // rotation 540 = 0x021C
    EXPECT_EQ(bytes[7], 0x02);
    EXPECT_EQ(bytes[8], 0xEF);                  // sequence, low byte first
    EXPECT_EQ(bytes[32], 7);                    // hat
    EXPECT_EQ(bytes[33], 0xCD);                 // combined, low byte first
    EXPECT_EQ(bytes[34], 0xAB);
    EXPECT_EQ(bytes.size(), 45U);
}

// The RGT's levers after combined: left at 37, right at 41, low byte first; clamped as the other axes.
TEST(StatePacket, CarriesTheLevers) {
    StatePacket packet = samplePacket();
    packet.report.leftLever = 0x1234;
    packet.report.rightLever = 0x5678;
    const auto bytes = encode(packet);
    EXPECT_EQ(bytes[37], 0x34);
    EXPECT_EQ(bytes[38], 0x12);
    EXPECT_EQ(bytes[41], 0x78);
    EXPECT_EQ(bytes[42], 0x56);
    const auto decoded = decodeStatePacket(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->report.leftLever, 0x1234);
    EXPECT_EQ(decoded->report.rightLever, 0x5678);
    packet.report.rightLever = 70000;
    EXPECT_EQ(decodeStatePacket(encode(packet))->report.rightLever, 65535);
}

TEST(StatePacket, CombinedIsClampedLikeTheOtherAxes) {
    StatePacket packet = samplePacket();
    packet.report.combined = -7;
    const auto decoded = decodeStatePacket(encode(packet));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->report.combined, 0);
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
}  // namespace wheelbri
