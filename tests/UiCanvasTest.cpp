// SPDX-License-Identifier: GPL-3.0-or-later
#include "UiWindow.hpp"

#include <gtest/gtest.h>

namespace wheelbri::service::ui {
namespace {

constexpr int kW = 960;   // the app's design size
constexpr int kH = 748;

TEST(UiCanvas, TheDesignSizeIsTheDesign) {
    const Letterbox box = fitCanvas(kW, kH, kW, kH);
    EXPECT_FLOAT_EQ(box.scale, 1.0F);
    EXPECT_FLOAT_EQ(box.width, 960.0F);
    EXPECT_FLOAT_EQ(box.height, 748.0F);
    EXPECT_FLOAT_EQ(box.offsetX, 0.0F);
    EXPECT_FLOAT_EQ(box.offsetY, 0.0F);
}

TEST(UiCanvas, TheSameShapeScalesWithNoRoomAdded) {
    const Letterbox box = fitCanvas(kW * 13 / 10, kH * 13 / 10, kW, kH, 1.5F);   // the app's first size
    EXPECT_NEAR(box.scale, 1.3F, 0.002F);
    EXPECT_NEAR(box.width, 960.0F, 1.0F);
    EXPECT_NEAR(box.height, 748.0F, 1.0F);
}

TEST(UiCanvas, AWiderWindowGivesRoomNotBars) {
    // 16:9 at 1.3: the height sets the scale, the width becomes room (no bars at the sides).
    const Letterbox box = fitCanvas(1920, 972, kW, kH, 1.5F);
    EXPECT_NEAR(box.scale, 972.0F / 748.0F, 0.001F);
    EXPECT_NEAR(box.width, 1920.0F / box.scale, 1.0F);
    EXPECT_GT(box.width, 1400.0F);
    EXPECT_NEAR(box.height, 748.0F, 1.0F);
    EXPECT_FLOAT_EQ(box.offsetX, 0.0F);
}

TEST(UiCanvas, ATallerWindowGivesRowsNotBars) {
    const Letterbox box = fitCanvas(960, 1200, kW, kH, 1.5F);
    EXPECT_FLOAT_EQ(box.scale, 1.0F);
    EXPECT_FLOAT_EQ(box.width, 960.0F);
    EXPECT_FLOAT_EQ(box.height, 1200.0F);
}

TEST(UiCanvas, PastTheLargestZoomBothSidesGrow) {
    // A 4K screen at 100 %: text at 1.5x, the rest is room both ways.
    const Letterbox box = fitCanvas(3840, 2100, kW, kH, 1.5F);
    EXPECT_FLOAT_EQ(box.scale, 1.5F);
    EXPECT_FLOAT_EQ(box.width, 2560.0F);
    EXPECT_FLOAT_EQ(box.height, 1400.0F);
}

TEST(UiCanvas, ASmallerWindowShowsTheDesignSmaller) {
    // Only when the screen is smaller than the smallest window: everything scaled down, never cut.
    const Letterbox box = fitCanvas(720, 600, kW, kH, 1.5F);
    EXPECT_FLOAT_EQ(box.scale, 0.75F);
    EXPECT_FLOAT_EQ(box.width, 960.0F);
    EXPECT_GE(box.height, 748.0F);
}

TEST(UiCanvas, NothingToFitKeepsTheDesign) {
    for (const auto& [w, h] : {std::pair{0, 600}, std::pair{800, 0}, std::pair{-5, -5}}) {
        const Letterbox box = fitCanvas(w, h, kW, kH, 1.5F);
        EXPECT_FLOAT_EQ(box.scale, 1.0F);
        EXPECT_FLOAT_EQ(box.width, 960.0F);
        EXPECT_FLOAT_EQ(box.height, 748.0F);
    }
}

}  // namespace
}  // namespace wheelbri::service::ui
