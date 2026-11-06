// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "UiWindow.hpp"

#include <string_view>

namespace wheelbri::service::style {

// The window's look, shared by every view (round 74, from the UX sketches): a near-black background, flat
// mid-grey rows, darker fields with a thin edge, buttons drawn as a grey outline only, and one colour per meaning:
// blue selected or an action, green fine, amber check this, red broken, violet yours; the rest grey. Views take their
// colours from here (their own kX constants are aliases), so the look changes in one place.
inline constexpr ui::Colour kBackground{22, 24, 28};
inline constexpr ui::Colour kPanel{42, 46, 54};         // a row, an unselected tab, a bar's track
inline constexpr ui::Colour kRule{44, 48, 56};          // a separating line; a greyed button's edge
inline constexpr ui::Colour kField{34, 37, 43};         // behind a value (field, source, session chip)
inline constexpr ui::Colour kFieldEdge{51, 56, 66};     // a field's edge
inline constexpr ui::Colour kBand{32, 36, 43};          // behind a section's heading
inline constexpr ui::Colour kBox{29, 32, 38};           // a framed box (checklist, dialog body)
inline constexpr ui::Colour kPopup{44, 49, 58};         // a popup, a tooltip
inline constexpr ui::Colour kFrame{90, 96, 106};        // a button's edge, a checkbox's frame
inline constexpr ui::Colour kSplit{34, 38, 46};         // the bar between a list and its page
inline constexpr ui::Colour kSplitEdge{58, 63, 72};

inline constexpr ui::Colour kText{212, 214, 218};
inline constexpr ui::Colour kDim{168, 173, 182};        // help, units, labels (bright enough to read)
inline constexpr ui::Colour kFaint{138, 143, 152};      // tags at a row's right, greyed labels
inline constexpr ui::Colour kHeading{108, 182, 255};    // blue: selected, an action, page titles
inline constexpr ui::Colour kHeadingTint{29, 48, 71};   // behind a small blue action at rest (blue when pointed at)
inline constexpr ui::Colour kActive{61, 187, 92};       // green: fine, running
inline constexpr ui::Colour kCaution{224, 160, 32};     // amber: check this, yours
inline constexpr ui::Colour kCautionBack{42, 36, 22};   // behind an amber notice
inline constexpr ui::Colour kMine{180, 140, 255};       // violet: yours (a value you changed, or set for one game)
inline constexpr ui::Colour kWarning{232, 86, 76};      // red: broken
inline constexpr ui::Colour kWarningBack{46, 27, 28};   // behind a red notice
inline constexpr ui::Colour kDark{16, 18, 20};          // text on a blue or green fill

// The Mapping tab's Shadow view (ShadowView.hpp): the target drawn as flat shapes, a shade or two off the background;
// what is pressed lights in kActive, a control nothing drives sinks to kShadowOff.
inline constexpr ui::Colour kShadow{52, 57, 67};        // the device's body, the rim
inline constexpr ui::Colour kShadowEdge{70, 76, 88};    // its edges; a paddle at rest
inline constexpr ui::Colour kShadowDeep{36, 40, 47};    // holes, bolts, the gate's slots
inline constexpr ui::Colour kShadowPanel{46, 50, 59};   // the shifter's button panel and boot
inline constexpr ui::Colour kSteel{58, 63, 74};         // spokes, pedal faces
inline constexpr ui::Colour kShadowControl{66, 72, 84}; // a control something drives, at rest
inline constexpr ui::Colour kShadowOff{36, 39, 46};     // a control nothing drives (sunk into the panel)
inline constexpr ui::Colour kShadowRed{128, 50, 50};    // a red button at rest
inline constexpr ui::Colour kShadowRedOff{70, 44, 48};  // a red button nothing drives
inline constexpr ui::Colour kShadowBlack{24, 26, 30};   // a black button at rest
inline constexpr ui::Colour kShadowKnob{30, 32, 38};    // the gear knob
inline constexpr ui::Colour kShadowKnobTop{64, 68, 78}; // its highlight
inline constexpr ui::Colour kActiveDeep{28, 104, 52};   // holes in a lit pedal
inline constexpr ui::Colour kShadowConsole{48, 52, 61}; // the G920's black wings
inline constexpr ui::Colour kAluminium{84, 90, 102};    // the G920's brushed plate and spokes
inline constexpr ui::Colour kShadowKey{150, 155, 166};  // the G920's light keycaps (View, Menu, LSB, RSB)
inline constexpr ui::Colour kShadowLamp{220, 220, 220}; // the little light over its Xbox button
// The letters on the G920's black A B X Y: Xbox's green, red, blue and yellow.
inline constexpr ui::Colour kLetterA{120, 190, 70};
inline constexpr ui::Colour kLetterB{205, 70, 70};
inline constexpr ui::Colour kLetterX{70, 150, 225};
inline constexpr ui::Colour kLetterY{215, 200, 70};

// A section's heading band: a darker strip across `w` behind the heading text at (x + 6, y), with a blue edge at its
// left. Draw it before the heading.
inline void headingBand(ui::UiWindow& window, const float x, const float y, const float w) {
    window.fill(x, y - 4, w, 16, kBand);
    window.fill(x, y - 4, 3, 16, kHeading);
}

// A section's heading text on its band: bold, in the text colour (the band's blue edge marks the section, so blue
// keeps meaning selected or an action).
inline void heading(ui::UiWindow& window, const float x, const float y, const std::string_view label) {
    window.heading(x, y, label, kText);
}

// A button: a grey outline on whatever is behind it, the label in the text colour; `on` fills it blue with dark
// text; greyed (not `enabled`) the edge and label fade. The caller adds the click.
inline void button(ui::UiWindow& window, const float x, const float y, const float w, const float h,
                   const std::string_view label, const bool on, const bool enabled) {
    if (on && enabled) window.fill(x, y, w, h, kHeading);
    window.outline(x, y, w, h, !enabled ? kRule : on ? kHeading : kFrame);
    const float textX = x + (w - static_cast<float>(label.size()) * ui::kCharWidth) / 2;
    window.text(textX, y + (h - ui::kCharHeight) / 2, label, !enabled ? kFaint : on ? kDark : kText);
}

// A switch as a checkbox: its frame (x, y, w, 16), the box at its left (filled when `on`) and `label`. Greyed when
// not `enabled`. The caller adds the click.
inline void checkbox(ui::UiWindow& window, const float x, const float y, const float w, const std::string_view label,
                     const bool on, const bool enabled) {
    window.outline(x, y, w, 16, enabled ? kFrame : kRule);
    window.outline(x + 5, y + 3, 10, 10, enabled ? kHeading : kFrame);
    if (on) window.fill(x + 7, y + 5, 6, 6, enabled ? kHeading : kFrame);
    window.text(x + 22, y + 4, label, enabled ? kText : kDim);
}

}  // namespace wheelbri::service::style
