// The app's window on Windows (service/src/UiWindowWin32.cpp), a real one: opened, drawn into, and fed the messages
// Windows sends it (posted to it here, as Windows would), read back through pump(). Skipped where no window can be
// opened (no desktop; under Wine: no X display, run it inside xvfb-run).
#include "UiWindow.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

using namespace wheelbri::service;

constexpr int kDesignW = 960;
constexpr int kDesignH = 748;

class Win32Window : public ::testing::Test {
protected:
    void SetUp() override {
        title_ = "wheelbri-test-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::string error;
        window_ = ui::openWindow(title_, kDesignW, kDesignH, kDesignW, kDesignH, error);
        if (window_ == nullptr) GTEST_SKIP() << "no window here: " << error;
        hwnd_ = FindWindowW(L"wheelbri-ui", std::wstring(title_.begin(), title_.end()).c_str());
        ASSERT_NE(hwnd_, nullptr);
        window_->pump();
    }

    // Posts `message` and returns what the next pump() hands over.
    ui::WindowInput post(const UINT message, const WPARAM wParam = 0, const LPARAM lParam = 0) {
        PostMessageW(hwnd_, message, wParam, lParam);
        return window_->pump();
    }

    std::string title_;
    std::unique_ptr<ui::UiWindow> window_;
    HWND hwnd_ = nullptr;
};

// Open, it is shown, says what it draws with, and its canvas is at least the design size.
TEST_F(Win32Window, OpensAtLeastAsBigAsTheDesign) {
    EXPECT_TRUE(window_->visible());
    EXPECT_GE(window_->width(), static_cast<float>(kDesignW));
    EXPECT_GE(window_->height(), static_cast<float>(kDesignH));
    EXPECT_NE(window_->describe().find("Win32 GDI"), std::string::npos);
    ShowWindow(hwnd_, SW_MINIMIZE);
    window_->pump();
    EXPECT_FALSE(window_->visible());   // minimised: drawing is skipped
    ShowWindow(hwnd_, SW_RESTORE);
}

// A frame is drawn (fills, outlines, text, an origin moved) and drawn again unchanged; empty text and a rectangle
// with no size are nothing. A painted window repaints from its buffer.
TEST_F(Win32Window, DrawsFrames) {
    for (int frame = 0; frame < 2; ++frame) {
        window_->begin({22, 24, 28});
        window_->setOrigin(10, 20);
        window_->fill(0, 0, 100, 16, {42, 46, 54});
        window_->fill(0, 0, 0, 16, {255, 0, 0});
        window_->fill(0, 0, 10, -1, {255, 0, 0});
        window_->outline(0, 0, 100, 16, {90, 96, 106});
        window_->text(4, 4, "Steering", {212, 214, 218});
        window_->text(4, 4, "", {212, 214, 218});
        window_->setOrigin(0, 0);
        window_->present();
    }
    window_->begin({22, 24, 28});
    window_->text(4, 4, "changed", {212, 214, 218});
    window_->present();
    InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateWindow(hwnd_);
    window_->pump();
    SUCCEED();
}

// Keys: Escape and the navigation keys as WheelBri's own codes, letters in lower case and not when repeated, other
// keys not at all; typed characters (printable ASCII, Backspace, Enter) as text, others dropped.
TEST_F(Win32Window, KeysAndText) {
    const std::pair<WPARAM, char> named[] = {
        {VK_ESCAPE, ui::kEscape}, {VK_PRIOR, ui::kPageUp}, {VK_NEXT, ui::kPageDown}, {VK_HOME, ui::kHome},
        {VK_END, ui::kEnd},       {VK_UP, ui::kUp},        {VK_DOWN, ui::kDown},     {VK_LEFT, ui::kLeft},
        {VK_RIGHT, ui::kRight}};
    for (const auto& [key, code] : named) {
        EXPECT_EQ(post(WM_KEYDOWN, key).keys, std::string(1, code)) << key;
    }
    EXPECT_EQ(post(WM_KEYDOWN, 'M').keys, "m");
    EXPECT_EQ(post(WM_KEYDOWN, 'M', 1L << 30).keys, "");   // held: a repeat
    EXPECT_EQ(post(WM_KEYDOWN, VK_F5).keys, "");
    EXPECT_EQ(post(WM_KEYDOWN, '5').keys, "");
    EXPECT_EQ(post(WM_CHAR, 'A').text, "A");
    EXPECT_EQ(post(WM_CHAR, VK_BACK).text, std::string(1, ui::kBackspace));
    EXPECT_EQ(post(WM_CHAR, VK_RETURN).text, std::string(1, ui::kEnter));
    EXPECT_EQ(post(WM_CHAR, 0x1B).text, "");    // a control character
    EXPECT_EQ(post(WM_CHAR, 0xE9).text, "");    // not ASCII
}

// The wheel in notches (a fine-grained wheel's remainder kept for the next notch), Shift + wheel and the tilt wheel
// sideways.
TEST_F(Win32Window, Wheels) {
    EXPECT_EQ(post(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA)).wheel, 1);
    EXPECT_EQ(post(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA))).wheel, -2);
    EXPECT_EQ(post(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2)).wheel, 0);
    EXPECT_EQ(post(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2)).wheel, 1);   // the two halves make a notch
    EXPECT_EQ(post(WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, WHEEL_DELTA)).hwheel, -1);
    EXPECT_EQ(post(0x020E, MAKEWPARAM(0, WHEEL_DELTA)).hwheel, 1);   // WM_MOUSEHWHEEL
}

// A press is a click on the canvas and starts a drag, a move tracks the pointer (inside the window until it leaves),
// a release or another window taking the mouse ends the drag.
TEST_F(Win32Window, PointerAndClicks) {
    ui::WindowInput input = post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 50));
    ASSERT_EQ(input.clicks.size(), 1U);
    EXPECT_TRUE(input.leftDown);
    EXPECT_GT(input.clicks[0].x, 0.0F);
    input = post(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 60));
    EXPECT_GT(input.pointer.x, input.clicks.empty() ? 0.0F : -1.0F);
    EXPECT_TRUE(input.clicks.empty());
    input = post(WM_LBUTTONUP, 0, MAKELPARAM(210, 60));
    EXPECT_FALSE(input.leftDown);
    post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    EXPECT_FALSE(post(WM_CAPTURECHANGED).leftDown);
    // The posted button-down really took the mouse (SetCapture): let it go, or every move of the real mouse anywhere
    // on the screen comes to this window during the pumps below and overwrites the posted position.
    ReleaseCapture();
    EXPECT_FALSE(post(WM_MOUSELEAVE).pointerInside);
    input = post(WM_MOUSEMOVE, 0, MAKELPARAM(-5, -5));   // coordinates left of and above the window
    EXPECT_LT(input.pointer.x, 0.0F);
}

// The close button only says so (the owner closes the window); logoff asks first (always yes, after the owner's
// handler) and then ends the session: the end handler runs and the window says it is closing. An end that was called
// off changes nothing.
TEST_F(Win32Window, CloseAndLogoff) {
    EXPECT_FALSE(window_->pump().close);
    EXPECT_TRUE(post(WM_CLOSE).close);
    int asked = 0;
    int ended = 0;
    window_->onQueryEndSession([&] { ++asked; });
    window_->onEndSession([&] { ++ended; });
    EXPECT_EQ(SendMessageW(hwnd_, WM_QUERYENDSESSION, 0, 0), TRUE);
    EXPECT_EQ(asked, 1);
    SendMessageW(hwnd_, WM_ENDSESSION, FALSE, 0);
    EXPECT_EQ(ended, 0);
    SendMessageW(hwnd_, WM_ENDSESSION, TRUE, 0);
    EXPECT_EQ(ended, 1);
    EXPECT_TRUE(window_->pump().close);
    window_->onEndSession([] { throw std::runtime_error("the session would not stop"); });
    SendMessageW(hwnd_, WM_ENDSESSION, TRUE, 0);   // logged, not thrown through Windows
    window_->onEndSession([] { throw 42; });
    SendMessageW(hwnd_, WM_ENDSESSION, TRUE, 0);
}

// A bigger window gives a bigger canvas (room, not bars); Windows asks for the size limits (at least kMinZoom times
// the design) and a new display scale moves the window to the size Windows suggests.
TEST_F(Win32Window, ResizeAndScale) {
    const float before = window_->width();
    MINMAXINFO limits{};
    SendMessageW(hwnd_, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
    EXPECT_GT(limits.ptMinTrackSize.x, 0);
    EXPECT_GE(limits.ptMaxTrackSize.x, limits.ptMinTrackSize.x);
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    RECT suggested{rect.left, rect.top, rect.left + (rect.right - rect.left) * 3 / 2, rect.bottom};
    SendMessageW(hwnd_, 0x02E0, MAKEWPARAM(96, 96), reinterpret_cast<LPARAM>(&suggested));   // WM_DPICHANGED
    window_->pump();
    window_->begin({0, 0, 0});
    EXPECT_GE(window_->width(), before);
    EXPECT_EQ(SendMessageW(hwnd_, WM_ERASEBKGND, 0, 0), 1);
}

// A notification is a balloon on WheelBri's icon in the notification area: the first adds the icon, the next ones
// change it, long texts are cut with dots. Where there is no notification area (Wine without a tray) it says so.
TEST_F(Win32Window, Notifications) {
    const bool first = window_->notify("WheelBri", "NFS Unbound does not support the G923");
    const bool second = window_->notify(std::string(200, 't'), std::string(400, 'x'));
    if (first) {
        EXPECT_TRUE(second);
    }
}

}  // namespace
