// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <utility>
#include <vector>

namespace wheelbri::service {

// The places a view's last draw made clickable, and what each does: added while drawing (clear() at its start), the
// topmost (the last added) taking a click. A click runs one action and empties the list: it is stale until the next
// draw, so a second click before it does nothing.
class HitList {
public:
    void clear() { hits_.clear(); }
    void add(const float x, const float y, const float w, const float h, std::function<void()> action) {
        hits_.push_back({x, y, w, h, std::move(action)});
    }
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
};

}  // namespace wheelbri::service
