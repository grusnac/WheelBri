// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/AxisTuning.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using namespace wheelbri;

TEST(AxisTuning, ParsesTypedNumbers) {
    EXPECT_EQ(tuning::parseNumber("180"), 180.0F);
    EXPECT_EQ(tuning::parseNumber(" 2.5 "), 2.5F);
    EXPECT_EQ(tuning::parseNumber("5%"), 5.0F);
    EXPECT_EQ(tuning::parseNumber("5 %"), 5.0F);
    EXPECT_EQ(tuning::parseNumber("180 deg"), 180.0F);
    EXPECT_EQ(tuning::parseNumber(".5"), 0.5F);
    EXPECT_EQ(tuning::parseNumber("-3"), -3.0F);
    for (const char* bad : {"", " ", "%", "abc", "1e3", "inf", "nan", "5%%", "1.2.3", "--1", "1-", "deg", "5 x"}) {
        EXPECT_FALSE(tuning::parseNumber(bad).has_value()) << bad;
    }
}

TEST(AxisTuning, FormatsWithoutTrailingZeros) {
    EXPECT_EQ(tuning::formatNumber(180.0F), "180");
    EXPECT_EQ(tuning::formatNumber(0.02F), "0.02");
    EXPECT_EQ(tuning::formatNumber(0.025F), "0.025");
    EXPECT_EQ(tuning::formatNumber(0.0F), "0");
    EXPECT_EQ(tuning::formatNumber(-0.0F), "0");
    EXPECT_EQ(tuning::formatPercent(0.02F), "2%");
    EXPECT_EQ(tuning::formatPercent(0.025F), "2.5%");
    EXPECT_EQ(tuning::formatPercent(1.0F), "100%");
}

TEST(AxisTuning, Ranges) {
    EXPECT_TRUE(tuning::rotationProblem(180.0F).empty());
    EXPECT_TRUE(tuning::rotationProblem(90.0F).empty());
    EXPECT_TRUE(tuning::rotationProblem(2520.0F).empty());
    EXPECT_FALSE(tuning::rotationProblem(89.0F).empty());
    EXPECT_FALSE(tuning::rotationProblem(3000.0F).empty());
}

// Numbers as people type them: tabs around, a unit with a space, digits only after the point, a lone "-" or ".".
TEST(AxisTuning, ParsesEdgesOfTypedNumbers) {
    EXPECT_EQ(tuning::parseNumber("\t900\t"), 900.0F);
    EXPECT_EQ(tuning::parseNumber("\t540 \tdeg "), 540.0F);
    EXPECT_EQ(tuning::parseNumber("7."), 7.0F);
    EXPECT_EQ(tuning::parseNumber("-.5%"), -0.5F);
    for (const char* bad : {"-", ".", "-.", "\t", "deg%", "% "}) {
        EXPECT_FALSE(tuning::parseNumber(bad).has_value()) << bad;
    }
    // Too big for a float: from_chars refuses it rather than giving infinity.
    EXPECT_FALSE(tuning::parseNumber(std::string(60, '9')).has_value());
}

// Whole numbers at zero decimals have no point to trim; a value that is not a number is never a rotation.
TEST(AxisTuning, FormatsWholeNumbersAndRefusesNonNumbers) {
    EXPECT_EQ(tuning::formatNumber(2.6F, 0), "3");
    EXPECT_EQ(tuning::formatNumber(-0.4F, 0), "0");
    EXPECT_EQ(tuning::formatNumber(1.5F, 3), "1.5");
    EXPECT_FALSE(tuning::rotationProblem(std::numeric_limits<float>::quiet_NaN()).empty());
    EXPECT_FALSE(tuning::rotationProblem(std::numeric_limits<float>::infinity()).empty());
}
