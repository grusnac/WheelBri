#pragma once

#include <span>
#include <vector>

namespace wheelbri {

// Filled shapes as horizontal runs, for a window that can only fill rectangles (UiWindow::fillPolygon and the others
// draw them through fill()): every row of `row` height whose middle is inside the shape gives one run per inside
// stretch. Rows sit on a grid of `row` from 0, so shapes drawn next to each other meet without gaps or overlaps, and
// with `row` one window pixel the edges are as sharp as the window can show. Portable, no allocations once `out` has
// grown.

struct ShapePoint {
    float x = 0.0F;
    float y = 0.0F;

    friend bool operator==(const ShapePoint&, const ShapePoint&) = default;
};

// One run: from x, w wide, on the row starting at y (`row` tall).
struct ShapeSpan {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;

    friend bool operator==(const ShapeSpan&, const ShapeSpan&) = default;
};

// A polygon (its corners in order, closed back to the first), filled even-odd: a corner list that crosses itself
// leaves the crossed part empty. Fewer than 3 corners: nothing. `out` is cleared first.
void polygonSpans(std::span<const ShapePoint> points, float row, std::vector<ShapeSpan>& out);

// An ellipse centred on (cx, cy) with radii rx, ry.
void ellipseSpans(float cx, float cy, float rx, float ry, float row, std::vector<ShapeSpan>& out);

// A ring: the circle of radius `outer` less the one of radius `inner` (both centred on cx, cy).
void ringSpans(float cx, float cy, float outer, float inner, float row, std::vector<ShapeSpan>& out);

}  // namespace wheelbri
