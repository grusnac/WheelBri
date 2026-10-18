#pragma once

#include "UiWindow.hpp"

#include <string_view>

namespace wheelbri::service::style {

// The window's look, shared by its views. One colour per meaning: blue selected or an action, green fine, amber check
// this, red broken; the rest grey.
inline constexpr ui::Colour kDim{150, 156, 166};        // help, units, labels (bright enough to read)
inline constexpr ui::Colour kHeading{120, 190, 255};
inline constexpr ui::Colour kBand{32, 36, 44};          // behind a section's heading
inline constexpr ui::Colour kText{222, 226, 232};
inline constexpr ui::Colour kFrame{70, 76, 86};         // a checkbox's frame

// A section's heading band: a darker strip across `w` behind the heading text at (x + 6, y), with a blue edge at its
// left. Draw it before the heading.
inline void headingBand(ui::UiWindow& window, const float x, const float y, const float w) {
    window.fill(x, y - 3, w, 14, kBand);
    window.fill(x, y - 3, 2, 14, kHeading);
}

// A switch as a checkbox: its frame (x, y, w, 16), the box at its left (filled when `on`) and `label`. Greyed when
// not `enabled`. The caller adds the click.
inline void checkbox(ui::UiWindow& window, const float x, const float y, const float w, const std::string_view label,
                     const bool on, const bool enabled) {
    window.outline(x, y, w, 16, kFrame);
    window.outline(x + 5, y + 3, 10, 10, enabled ? kHeading : kFrame);
    if (on) window.fill(x + 7, y + 5, 6, 6, enabled ? kHeading : kFrame);
    window.text(x + 22, y + 4, label, enabled ? kText : kDim);
}

}  // namespace wheelbri::service::style
