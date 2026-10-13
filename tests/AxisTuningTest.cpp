#include "wheelbri/AxisTuning.hpp"

#include <gtest/gtest.h>

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
