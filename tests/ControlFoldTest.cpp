// SPDX-License-Identifier: GPL-3.0-or-later
// The Rig tab's folding of unnamed controls (core/src/ControlFold.cpp).

#include "wheelbri/ControlFold.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

TEST(ControlFold, NamedOnesStayTheRestFold) {
    const std::vector<bool> named{true, false, true, false, false, true, true, false};
    const ControlFold folded = foldUnnamed(named, true);
    EXPECT_EQ(folded.shown, (std::vector<int>{0, 2, 5, 6}));
    EXPECT_EQ(folded.hidden, (std::vector<int>{1, 3, 4, 7}));
    const ControlFold all = foldUnnamed(named, false);
    EXPECT_EQ(all.shown, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_TRUE(all.hidden.empty());
    EXPECT_TRUE(foldUnnamed({}, true).shown.empty());
}

// From `foldFrom` on, unnamed controls fold even without folding (a Moza base's buttons past 63).
TEST(ControlFold, UnnamedOnesFromFoldFromFoldAnyway) {
    const std::vector<bool> named{false, false, true, false, false, true};
    const ControlFold folded = foldUnnamed(named, false, 3);
    EXPECT_EQ(folded.shown, (std::vector<int>{0, 1, 2, 5}));
    EXPECT_EQ(folded.hidden, (std::vector<int>{3, 4}));
    EXPECT_EQ(foldUnnamed(named, true, 3).shown, (std::vector<int>{2, 5}));
    EXPECT_EQ(foldUnnamed(named, false, named.size()).shown, (std::vector<int>{0, 1, 2, 3, 4, 5}));
}

TEST(ControlFold, RangesOfThreeOrMoreAreShort) {
    EXPECT_EQ(indexRanges({}), "");
    EXPECT_EQ(indexRanges({7}), "7");
    EXPECT_EQ(indexRanges({1, 3, 4, 7}), "1, 3, 4, 7");
    EXPECT_EQ(indexRanges({8, 9, 10, 11, 14, 15, 16, 17, 25, 26, 27, 28, 29, 30}), "8-11, 14-17, 25-30");
    EXPECT_EQ(indexRanges({0, 1, 2, 5, 6}), "0-2, 5, 6");
}

}  // namespace
}  // namespace wheelbri
