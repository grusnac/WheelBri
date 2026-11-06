// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/ShapeSpans.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace wheelbri {

namespace {

// The rows (their tops) whose middles lie in [top, bottom), on the grid of `row` from 0.
template <typename Each>
void forRows(const float top, const float bottom, const float row, Each&& each) {
    if (!(row > 0.0F) || !(bottom > top)) return;
    // The first row whose middle is at or below `top`.
    // Counted in whole rows (no sum of floats drifting off the grid).
    for (auto k = static_cast<long>(std::ceil(top / row - 0.5F)); (static_cast<float>(k) + 0.5F) * row < bottom; ++k) {
        const float y = static_cast<float>(k) * row;
        each(y, y + row * 0.5F);
    }
}

// Half the width of a circle (or ellipse with rx, ry) at `dy` from its centre; negative when outside it.
float halfWidth(const float rx, const float ry, const float dy) {
    if (!(ry > 0.0F) || std::fabs(dy) >= ry) return -1.0F;
    const float t = dy / ry;
    return rx * std::sqrt(1.0F - t * t);
}

}  // namespace

void polygonSpans(const std::span<const ShapePoint> points, const float row, std::vector<ShapeSpan>& out) {
    out.clear();
    if (points.size() < 3) return;
    float top = points[0].y;
    float bottom = points[0].y;
    for (const ShapePoint& p : points) {
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }
    // Crossings per row: a fixed buffer (a picture's shapes have a handful of corners); more are ignored.
    std::array<float, 64> crossings{};
    forRows(top, bottom, row, [&](const float y, const float middle) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const ShapePoint& a = points[i];
            const ShapePoint& b = points[(i + 1) % points.size()];
            // Half-open, so a corner exactly on the row's middle counts once.
            if ((a.y <= middle && b.y > middle) || (b.y <= middle && a.y > middle)) {
                if (count < crossings.size()) {
                    crossings[count++] = a.x + (middle - a.y) * (b.x - a.x) / (b.y - a.y);
                }
            }
        }
        std::sort(crossings.begin(), crossings.begin() + static_cast<std::ptrdiff_t>(count));
        for (std::size_t i = 0; i + 1 < count; i += 2) {
            if (crossings[i + 1] > crossings[i]) out.push_back({crossings[i], y, crossings[i + 1] - crossings[i]});
        }
    });
}

void ellipseSpans(const float cx, const float cy, const float rx, const float ry, const float row,
                  std::vector<ShapeSpan>& out) {
    out.clear();
    if (!(rx > 0.0F) || !(ry > 0.0F)) return;
    forRows(cy - ry, cy + ry, row, [&](const float y, const float middle) {
        const float half = halfWidth(rx, ry, middle - cy);
        if (half > 0.0F) out.push_back({cx - half, y, half * 2.0F});
    });
}

void ringSpans(const float cx, const float cy, const float outer, const float inner, const float row,
               std::vector<ShapeSpan>& out) {
    out.clear();
    if (!(outer > 0.0F)) return;
    const float hole = std::clamp(inner, 0.0F, outer);
    forRows(cy - outer, cy + outer, row, [&](const float y, const float middle) {
        const float dy = middle - cy;
        const float outerHalf = halfWidth(outer, outer, dy);
        if (outerHalf <= 0.0F) return;
        const float innerHalf = halfWidth(hole, hole, dy);
        if (innerHalf <= 0.0F) {
            out.push_back({cx - outerHalf, y, outerHalf * 2.0F});
        } else {
            out.push_back({cx - outerHalf, y, outerHalf - innerHalf});
            out.push_back({cx + innerHalf, y, outerHalf - innerHalf});
        }
    });
}

}  // namespace wheelbri
