#include "wheelbri/TestClutch.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace wheelbri {
namespace {

// 25 % pressed, byte for byte as the old ClutchProbe sent it.
constexpr std::array<std::uint8_t, 12> kQuarter = {0x57, 0x45, 0x43, 0x4C, 0x01, 0x00, 0x00, 0x00, 0xC4, 0x09, 0x00, 0x00};

TEST(ClutchPacket, DecodesTheBridgeToolsBytes) {
    const auto pressed = ClutchPacket::decode(kQuarter);
    ASSERT_TRUE(pressed.has_value());
    EXPECT_FLOAT_EQ(*pressed, 0.25F);
    EXPECT_EQ(ClutchPacket::encode(0.25F), kQuarter);
}

TEST(ClutchPacket, RejectsOtherBytes) {
    auto bad = kQuarter;
    bad[0] = 0;
    EXPECT_FALSE(ClutchPacket::decode(bad).has_value());
    EXPECT_FALSE(ClutchPacket::decode(std::span(kQuarter).first(11)).has_value());
    auto overRange = ClutchPacket::encode(1.0F);
    overRange[8] = 0xFF;
    overRange[9] = 0xFF;
    EXPECT_FLOAT_EQ(*ClutchPacket::decode(overRange), 1.0F);
}

TEST(TestClutch, CountsFor1500MsAfterTheLastPacket) {
    TestClutch clutch;
    EXPECT_FALSE(clutch.active(0).has_value());
    clutch.offer(0.6F, 1'000'000'000);
    ASSERT_TRUE(clutch.active(1'000'000'000).has_value());
    EXPECT_FLOAT_EQ(*clutch.active(2'500'000'000), 0.6F);
    EXPECT_FALSE(clutch.active(2'500'000'001).has_value());
    clutch.offer(1.7F, 3'000'000'000);
    EXPECT_FLOAT_EQ(*clutch.active(3'000'000'000), 1.0F);
}

// A packet from a newer ClutchProbe (another version) is not read as this one.
TEST(ClutchPacket, RejectsAnotherVersion) {
    auto newer = kQuarter;
    newer[4] = 0x02;
    EXPECT_FALSE(ClutchPacket::decode(newer).has_value());
    auto highByte = kQuarter;
    highByte[5] = 0x01;
    EXPECT_FALSE(ClutchPacket::decode(highByte).has_value());
}

}  // namespace
}  // namespace wheelbri
