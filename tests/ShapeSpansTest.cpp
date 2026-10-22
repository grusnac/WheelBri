#include "wheelbri/ShapeSpans.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace wheelbri;

namespace {

// The area the runs cover, `row` tall each.
float area(const std::vector<ShapeSpan>& spans, const float row) {
    float total = 0.0F;
    for (const ShapeSpan& s : spans) total += s.w * row;
    return total;
}

}  // namespace

TEST(ShapeSpans, ARectangleIsOneRunPerRow) {
    const std::array<ShapePoint, 4> square = {{{10, 20}, {30, 20}, {30, 24}, {10, 24}}};
    std::vector<ShapeSpan> spans;
    polygonSpans(square, 1.0F, spans);
    ASSERT_EQ(spans.size(), 4U);
    for (std::size_t i = 0; i < spans.size(); ++i) {
        EXPECT_EQ(spans[i], (ShapeSpan{10, 20.0F + static_cast<float>(i), 20}));
    }
}

TEST(ShapeSpans, ATriangleNarrowsToItsTip) {
    const std::array<ShapePoint, 3> triangle = {{{0, 0}, {20, 10}, {0, 20}}};
    std::vector<ShapeSpan> spans;
    polygonSpans(triangle, 1.0F, spans);
    ASSERT_EQ(spans.size(), 20U);
    EXPECT_NEAR(spans[0].w, 1.0F, 1e-4);     // the row 0..1, its middle 0.5 from the top corner
    EXPECT_NEAR(spans[9].w, 19.0F, 1e-4);    // the widest row, just above the tip's height
    EXPECT_NEAR(area(spans, 1.0F), 200.0F, 1e-3);   // half of 20 x 20
}

TEST(ShapeSpans, ACrossedPolygonLeavesItsInsideEmpty) {
    // A square drawn twice round (outer, then inner the other way) as one list: even-odd leaves a frame.
    const std::array<ShapePoint, 10> frame = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0},
                                               {3, 3}, {3, 7}, {7, 7}, {7, 3}, {3, 3}}};
    std::vector<ShapeSpan> spans;
    polygonSpans(frame, 1.0F, spans);
    EXPECT_NEAR(area(spans, 1.0F), 100.0F - 16.0F, 1e-3);
}

TEST(ShapeSpans, TooFewCornersDrawNothing) {
    const std::array<ShapePoint, 2> line = {{{0, 0}, {10, 10}}};
    std::vector<ShapeSpan> spans = {{1, 2, 3}};
    polygonSpans(line, 1.0F, spans);
    EXPECT_TRUE(spans.empty());
}

TEST(ShapeSpans, RowsSitOnOneGridSoNeighboursMeet) {
    // Two rectangles meeting at y = 5.3: the rows of both together cover each row once.
    const std::array<ShapePoint, 4> upper = {{{0, 0.2F}, {10, 0.2F}, {10, 5.3F}, {0, 5.3F}}};
    const std::array<ShapePoint, 4> lower = {{{0, 5.3F}, {10, 5.3F}, {10, 9.8F}, {0, 9.8F}}};
    std::vector<ShapeSpan> a;
    std::vector<ShapeSpan> b;
    polygonSpans(upper, 0.5F, a);
    polygonSpans(lower, 0.5F, b);
    std::vector<float> rows;
    for (const ShapeSpan& s : a) rows.push_back(s.y);
    for (const ShapeSpan& s : b) rows.push_back(s.y);
    for (std::size_t i = 1; i < rows.size(); ++i) EXPECT_FLOAT_EQ(rows[i] - rows[i - 1], 0.5F) << i;
}

TEST(ShapeSpans, AnEllipseCoversItsArea) {
    std::vector<ShapeSpan> spans;
    ellipseSpans(100, 100, 40, 20, 0.25F, spans);
    EXPECT_NEAR(area(spans, 0.25F), 3.14159265F * 40 * 20, 3.14159265F * 40 * 20 * 0.01F);
    for (const ShapeSpan& s : spans) EXPECT_NEAR(s.x + s.w / 2, 100.0F, 1e-3);   // centred
}

TEST(ShapeSpans, ARingHasAHole) {
    std::vector<ShapeSpan> spans;
    ringSpans(0, 0, 10, 6, 0.1F, spans);
    EXPECT_NEAR(area(spans, 0.1F), 3.14159265F * (100 - 36), 3.14159265F * 64 * 0.01F);
    // Through the middle: two runs, 4 wide each.
    bool middle = false;
    for (std::size_t i = 0; i + 1 < spans.size(); ++i) {
        if (spans[i].y == spans[i + 1].y && spans[i].y > -0.2F && spans[i].y < 0.0F) {
            middle = true;
            EXPECT_NEAR(spans[i].w, 4.0F, 0.01F);
            EXPECT_NEAR(spans[i + 1].x, 6.0F, 0.01F);
        }
    }
    EXPECT_TRUE(middle);
}

TEST(ShapeSpans, NoSizeNoRuns) {
    std::vector<ShapeSpan> spans;
    ellipseSpans(0, 0, 0, 5, 1.0F, spans);
    EXPECT_TRUE(spans.empty());
    ringSpans(0, 0, 5, 2, 0.0F, spans);
    EXPECT_TRUE(spans.empty());
}
