// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "UiWindow.hpp"

#include <string>
#include <vector>

namespace wheelbri::service {

// Hover text: the areas of the last draw that explain something, each with its text. The one under the mouse (the
// last one added there wins) is drawn in a box next to the pointer, over everything, wrapped to at most
// kMaxChars characters a line and kMaxLines lines. The tabs keep their bottom to one status line and put the rest here.
class HoverText {
public:
    static constexpr std::size_t kMaxChars = 64;
    static constexpr std::size_t kMaxLines = 6;

    void clear() { areas_.clear(); }
    void add(float x, float y, float w, float h, std::string text);
    // Where the mouse is (the tab's own pixels); `inside` false: it left the window, nothing shows.
    void setHover(float x, float y, bool inside);
    // The text under the mouse, or null; `first`: areas before this index are not looked at (a popup's own come
    // after the tab's it covers).
    [[nodiscard]] const std::string* under(std::size_t first = 0) const;
    [[nodiscard]] std::size_t size() const { return areas_.size(); }
    // Draws it: below and right of the pointer, moved in at the right edge (`canvasW`) and above it at `bottom`.
    void draw(ui::UiWindow& window, float canvasW, float bottom, std::size_t first = 0) const;
    // A text at the pointer (x, y) in that box: for a view with its own list of areas.
    static void drawBox(ui::UiWindow& window, float x, float y, const std::string& text, float canvasW, float bottom);
    // `text` in lines of at most `chars` (at spaces), at most kMaxLines; the last one cut with ".." if it goes on.
    [[nodiscard]] static std::vector<std::string> lines(const std::string& text, std::size_t chars);

private:
    struct Area {
        float x, y, w, h;
        std::string text;
    };
    std::vector<Area> areas_;
    float x_ = 0;
    float y_ = 0;
    bool inside_ = false;
};

}  // namespace wheelbri::service
