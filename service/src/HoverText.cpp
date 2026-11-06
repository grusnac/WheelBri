// SPDX-License-Identifier: GPL-3.0-or-later
#include "HoverText.hpp"

#include "UiStyle.hpp"

#include <algorithm>
#include <utility>

namespace wheelbri::service {
namespace {

constexpr ui::Colour kBox = style::kPopup;
constexpr ui::Colour kEdge = style::kHeading;
constexpr ui::Colour kText = style::kText;
constexpr float kLineH = 14;

}  // namespace

void HoverText::add(const float x, const float y, const float w, const float h, std::string text) {
    if (!text.empty()) areas_.push_back({x, y, w, h, std::move(text)});
}

void HoverText::setHover(const float x, const float y, const bool inside) {
    x_ = x;
    y_ = y;
    inside_ = inside;
}

const std::string* HoverText::under(const std::size_t first) const {
    if (!inside_) return nullptr;
    for (auto it = areas_.rbegin(); it != areas_.rend(); ++it) {
        if (static_cast<std::size_t>(areas_.rend() - it) <= first) break;
        if (x_ >= it->x && x_ < it->x + it->w && y_ >= it->y && y_ < it->y + it->h) return &it->text;
    }
    return nullptr;
}

void HoverText::draw(ui::UiWindow& window, const float canvasW, const float bottom, const std::size_t first) const {
    if (const std::string* const text = under(first)) drawBox(window, x_, y_, *text, canvasW, bottom);
}

void HoverText::drawBox(ui::UiWindow& window, const float x, const float y, const std::string& text,
                        const float canvasW, const float bottom) {
    const auto room = static_cast<std::size_t>(std::max(8.0F, (canvasW - 32) / ui::kCharWidth));
    const std::vector<std::string> shown = lines(text, std::min(kMaxChars, room));
    if (shown.empty()) return;
    std::size_t longest = 0;
    for (const std::string& line : shown) longest = std::max(longest, line.size());
    const float w = static_cast<float>(longest) * ui::kCharWidth + 16;
    const float h = static_cast<float>(shown.size()) * kLineH + 8;
    // Below and right of the pointer; moved in at the right edge, above the pointer at the bottom.
    float left = std::min(x + 12, canvasW - w - 2);
    float top = y + 18;
    if (top + h > bottom - 2) top = y - h - 6;
    left = std::max(left, 2.0F);
    top = std::max(top, 2.0F);
    window.fill(left, top, w, h, kBox);
    window.outline(left, top, w, h, kEdge);
    for (std::size_t i = 0; i < shown.size(); ++i) {
        window.text(left + 8, top + 6 + static_cast<float>(i) * kLineH, shown[i], kText);
    }
}

std::vector<std::string> HoverText::lines(const std::string& text, const std::size_t chars) {
    std::vector<std::string> out;
    if (chars < 3) return out;
    std::string rest = text;
    while (!rest.empty()) {
        if (out.size() + 1 == kMaxLines && rest.size() > chars) {
            out.push_back(rest.substr(0, chars - 2) + "..");
            break;
        }
        if (rest.size() <= chars) {
            out.push_back(rest);
            break;
        }
        std::size_t cut = rest.rfind(' ', chars);
        if (cut == std::string::npos || cut == 0) cut = chars;
        out.push_back(rest.substr(0, cut));
        rest = rest.substr(std::min(rest.size(), cut + (rest[cut] == ' ' ? 1 : 0)));
    }
    return out;
}

}  // namespace wheelbri::service
