// SPDX-License-Identifier: GPL-3.0-or-later
// A source device's pressed buttons, 0..127 (core/include/wheelbri/SourceButtons.hpp).

#include "wheelbri/SourceButtons.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace wheelbri {
namespace {

TEST(SourceButtons, SetTestAndResetBothWords) {
    SourceButtons b;
    EXPECT_TRUE(b.none());
    b.set(0);
    b.set(63);
    b.set(64);
    b.set(119);   // a Moza base's reverse gear
    b.set(127);
    EXPECT_TRUE(b.any());
    EXPECT_EQ(b.count(), 5);
    EXPECT_EQ(b, SourceButtons::fromBits(1ULL | (1ULL << 63), 1ULL | (1ULL << 55) | (1ULL << 63)));
    EXPECT_TRUE(b.test(119));
    EXPECT_FALSE(b.test(118));
    b.reset(119);
    EXPECT_FALSE(b.test(119));
    b.set(64, false);
    EXPECT_FALSE(b.test(64));
    b.clear();
    EXPECT_TRUE(b.none());
}

// Numbers outside 0..127 are never pressed and setting them changes nothing.
TEST(SourceButtons, OutOfRangeNumbersAreIgnored) {
    SourceButtons b;
    b.set(-1);
    b.set(128);
    b.set(1000);
    EXPECT_TRUE(b.none());
    b = SourceButtons::fromBits(~0ULL, ~0ULL);
    EXPECT_FALSE(b.test(-1));
    EXPECT_FALSE(b.test(128));
    EXPECT_TRUE(b.test(127));
}

TEST(SourceButtons, NextWalksWhatIsPressedAcrossTheWords) {
    SourceButtons b;
    for (const int n : {2, 63, 64, 112, 127}) b.set(n);
    std::vector<int> seen;
    for (int n = b.next(0); n >= 0; n = b.next(n + 1)) seen.push_back(n);
    EXPECT_EQ(seen, (std::vector<int>{2, 63, 64, 112, 127}));
    EXPECT_EQ(b.next(-5), 2);
    EXPECT_EQ(b.next(65), 112);
    EXPECT_EQ(b.next(128), -1);
    EXPECT_EQ(SourceButtons{}.next(0), -1);
}

TEST(SourceButtons, OperatorsWorkPerBit) {
    const SourceButtons a = SourceButtons::fromBits(0b0110, 0b01);
    const SourceButtons c = SourceButtons::fromBits(0b0011, 0b11);
    EXPECT_EQ(a & c, SourceButtons::fromBits(0b0010, 0b01));
    EXPECT_EQ(a | c, SourceButtons::fromBits(0b0111, 0b11));
    EXPECT_EQ(a ^ c, SourceButtons::fromBits(0b0101, 0b10));
    EXPECT_EQ(c & ~a, SourceButtons::fromBits(0b0001, 0b10));   // newly pressed: now and not before
    static_assert(SourceButtons::fromBits(0, 1ULL << 55).test(119));
}

}  // namespace
}  // namespace wheelbri
