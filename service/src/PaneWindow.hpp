// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "UiWindow.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace wheelbri::service::ui {

// A part of another window that a view draws into as if it were the whole window: the app's side by side (the
// Mapping tab's editor left, the Live tab right). place() sets where it is on its window's canvas (from `left`,
// `width` wide, the window's whole height); width() and height() are then that part's, every draw call lands moved by
// `left`. Placed over the whole canvas it is the window itself. The frame (begin, present) and the input (pump) stay
// the window's: the app runs them and hands each view its clicks in the view's own coordinates (less `left`).
class PaneWindow final : public UiWindow {
public:
    explicit PaneWindow(UiWindow& window) : window_(window) { place(0.0F, window.width()); }

    void place(const float left, const float width) {
        left_ = left;
        canvasWidth_ = width;
        canvasHeight_ = window_.height();
    }
    [[nodiscard]] float left() const { return left_; }

    WindowInput pump() override { return {}; }   // the window's own pump() is the app's
    [[nodiscard]] bool visible() const override { return window_.visible(); }
    void begin(Colour /*background*/) override {}
    void fill(const float x, const float y, const float w, const float h, const Colour colour) override {
        window_.fill(x + originX_ + left_, y + originY_, w, h, colour);
    }
    void outline(const float x, const float y, const float w, const float h, const Colour colour) override {
        window_.outline(x + originX_ + left_, y + originY_, w, h, colour);
    }
    void text(const float x, const float y, const std::string_view value, const Colour colour) override {
        window_.text(x + originX_ + left_, y + originY_, value, colour);
    }
    void heading(const float x, const float y, const std::string_view value, const Colour colour) override {
        window_.heading(x + originX_ + left_, y + originY_, value, colour);
    }
    void present() override {}
    [[nodiscard]] float pixelSize() const override { return window_.pixelSize(); }
    bool openFolder(const std::filesystem::path& folder, std::string& error) override {
        return window_.openFolder(folder, error);
    }
    bool pickFolder(const std::string& title, std::filesystem::path& folder, std::string& error) override {
        return window_.pickFolder(title, folder, error);
    }
    bool notify(const std::string& title, const std::string& text) override { return window_.notify(title, text); }
    [[nodiscard]] std::string describe() const override { return window_.describe(); }

private:
    UiWindow& window_;
    float left_ = 0.0F;
};

}  // namespace wheelbri::service::ui
