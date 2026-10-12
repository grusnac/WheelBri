#include "wheelbri/DrawList.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

constexpr DrawList::Rgb kBlack{0, 0, 0};
constexpr DrawList::Rgb kWhite{255, 255, 255};

void frame(DrawList& list, const char* text, const float x = 10.0F) {
    list.begin(kBlack);
    list.fill(x, 20.0F, 30.0F, 40.0F, kWhite);
    list.text(5.0F, 6.0F, text, kWhite);
}

TEST(DrawList, TheSameFrameTwiceIsTheSame) {
    DrawList a;
    DrawList b;
    frame(a, "steer 0.50");
    frame(b, "steer 0.50");
    EXPECT_TRUE(a.sameAs(b));
}

TEST(DrawList, AnyDifferenceCounts) {
    DrawList a;
    DrawList b;
    frame(a, "steer 0.50");
    frame(b, "steer 0.51");
    EXPECT_FALSE(a.sameAs(b));
    frame(b, "steer 0.50", 11.0F);
    EXPECT_FALSE(a.sameAs(b));
    frame(b, "steer 0.50");
    b.begin(kWhite);
    b.fill(10.0F, 20.0F, 30.0F, 40.0F, kWhite);
    b.text(5.0F, 6.0F, "steer 0.50", kWhite);
    EXPECT_FALSE(a.sameAs(b));   // the background
}

TEST(DrawList, AShorterFrameIsDifferentEvenWithOldCommandsKept) {
    DrawList a;
    DrawList b;
    frame(a, "x");
    frame(b, "x");
    b.text(0.0F, 0.0F, "one more", kWhite);
    EXPECT_FALSE(a.sameAs(b));
    frame(b, "x");   // the extra command's memory is kept, but it is not part of this frame
    EXPECT_TRUE(a.sameAs(b));
    EXPECT_EQ(b.commands().size(), 2U);
}

TEST(DrawList, SwapKeepsWhatWasOnScreen) {
    DrawList drawing;
    DrawList shown;
    frame(drawing, "a");
    drawing.swap(shown);
    frame(drawing, "a");
    EXPECT_TRUE(drawing.sameAs(shown));
    frame(drawing, "b");
    EXPECT_FALSE(drawing.sameAs(shown));
    EXPECT_EQ(shown.commands()[1].text, "a");
}

}  // namespace
}  // namespace wheelbri
