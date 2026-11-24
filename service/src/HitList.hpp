// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace wheelbri::service {

// Part of a view drawn scrolled: its content's y plus `dy` on the canvas, seen only from `top` to `bottom`. The places
// it makes clickable (and its hover texts) are moved and cut to what is seen; nothing of a place out of view is kept.
struct HitView {
    float dy = 0.0F;
    float top = 0.0F;
    float bottom = 0.0F;
    bool on = false;
    // Moves and cuts (y, h); false when none of it is seen. Off: unchanged.
    [[nodiscard]] bool apply(float& y, float& h) const {
        if (!on) return true;
        const float y0 = std::max(y + dy, top);
        const float y1 = std::min(y + dy + h, bottom);
        if (y1 <= y0) return false;
        y = y0;
        h = y1 - y0;
        return true;
    }
};

// The places a view's last draw made clickable, and what each does: added while drawing (clear() at its start), the
// topmost (the last added) taking a click. A click runs one action and empties the list: it is stale until the next
// draw, so a second click before it does nothing.
class HitList {
public:
    void clear() { hits_.clear(); }
    void add(const float x, float y, const float w, float h, std::function<void()> action) {
        if (!view_.apply(y, h)) return;
        hits_.push_back({x, y, w, h, std::move(action)});
    }
    // What add() does to the places from now on (HitView); off again with a default HitView.
    void setView(const HitView& view) { view_ = view; }
    // Whether a place took the click at (x, y) (its action has run then).
    bool dispatch(const float x, const float y) {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) {
                const auto action = it->action;   // a copy: the list belongs to the last draw
                hits_.clear();
                action();
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool empty() const { return hits_.empty(); }

private:
    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };
    std::vector<Hit> hits_;
    HitView view_;
};

}  // namespace wheelbri::service
