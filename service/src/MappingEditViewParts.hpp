// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the parts of MappingEditView (MappingEditView.cpp: the mapping; MappingEditGames.cpp: the Games page;
// MappingEditPickers.cpp: the popups) share: their colours, sizes and small text helpers. Only those files include it.

#include "UiStyle.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/Rig.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wheelbri::service::editview {

// "G923" from "g923": a target's config name as the window shows it.
inline std::string upperName(std::string name) {
    for (char& c : name) c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    return name;
}

inline const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.mapping");
    return logger;
}

using Colour = ui::Colour;

// The same colours as the rest of the window (App.cpp, WheelSetupView.cpp).
constexpr Colour kPanel = style::kPanel;
constexpr Colour kStripe = style::kField;
constexpr Colour kText = style::kText;
constexpr Colour kDim = style::kDim;
constexpr Colour kHeading = style::kHeading;
constexpr Colour kActive = style::kActive;
constexpr Colour kWarning = style::kWarning;
constexpr Colour kCaution = style::kCaution;
constexpr Colour kMine = style::kMine;
constexpr Colour kDark = style::kDark;

constexpr std::uint64_t kDevicesEveryMs = 2000;
constexpr std::array<const char*, 4> kAxisNames = {"Steering", "Throttle", "Brake", "Clutch"};
constexpr std::array<const char*, 4> kHatNames = {"Up", "Right", "Down", "Left"};

// The popups: 24 in from the sides and 56 from the top; as wide and tall as the canvas allows (popupW, popupH), their
// entries in more columns when wider (popupColumns).
// Between a row's label and its field, everywhere on the tab: one and a half characters.
constexpr float kLabelGap = 12;
// A button row's label: its number and at most 10 characters of its name ("24 Dial Enter").
constexpr std::size_t kButtonLabelChars = 3 + 10;
constexpr float kPlusW = 24;    // a button row's "+" (adds a secondary)
constexpr float kChipGap = 4;   // between a button's secondary chips
constexpr float kPopupX = 24;
constexpr float kPopupY = 56;


// The game full lock slider: 90 to 1080 degrees, in steps of 10 (typed values go up to tuning::kMaxRotationDeg).
constexpr float kSliderX = 132;
constexpr float kSliderMinDeg = 90.0F;
constexpr float kSliderMaxDeg = 1080.0F;
constexpr float kSliderStepDeg = 10.0F;
constexpr std::array<float, 7> kPresetsDeg = {180.0F, 270.0F, 360.0F, 540.0F, 720.0F, 900.0F, 1080.0F};
// The centring spring's usual values (percent; 0 = off); any whole number from 0 to 100 can be typed.
constexpr std::array<float, 6> kSpringPresets = {0.0F, 10.0F, 25.0F, 50.0F, 75.0F, 100.0F};

inline std::string percent(const float value) { return tuning::formatNumber(value) + "%"; }
constexpr std::size_t kMaxTyped = 12;

inline std::string fit(std::string text, const std::size_t chars) {
    if (text.size() > chars) text = text.substr(0, chars - 2) + "..";
    return text;
}

// `text` in lines of at most `chars`, broken at spaces; at most `maxLines`, the last one cut with "..".
inline std::vector<std::string> wrap(const std::string& text, const std::size_t chars, const std::size_t maxLines) {
    std::vector<std::string> lines;
    std::string rest = text;
    while (!rest.empty() && lines.size() < maxLines) {
        if (rest.size() <= chars || lines.size() + 1 == maxLines) {
            lines.push_back(fit(rest, chars));
            break;
        }
        std::size_t cut = rest.rfind(' ', chars);
        if (cut == std::string::npos || cut == 0) cut = chars;
        lines.push_back(rest.substr(0, cut));
        rest = rest.substr(std::min(rest.size(), cut + 1));
    }
    return lines;
}

inline std::string degrees(const float value) { return tuning::formatNumber(value, 1); }

// `path` is UTF-8 (as the picker keeps it).
inline std::string readFile(const std::string& path) {
    const std::filesystem::path file = pathFromUtf8(path);
    return readFileText(file.empty() ? std::filesystem::path(path) : file).value_or(std::string());
}

inline DeviceId idOf(const DeviceLabels& labels) { return {labels.vendorId, labels.productId}; }

// What a rig control is called in this tab: its name (empty when it has none: the caller says its number).
inline std::string controlName(const RigControl& control) { return control.displayName(); }

inline std::string deviceName(const DeviceLabels& labels) {
    return labels.name.empty() ? deviceId(labels.vendorId, labels.productId) : labels.name;
}

}  // namespace wheelbri::service::editview
