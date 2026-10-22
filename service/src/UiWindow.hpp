#pragma once

#include "wheelbri/ShapeSpans.hpp"

#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::service::ui {

// The `--ui` window's drawing surface, implemented with each system's own tools:
//   Windows: Win32 + GDI (UiWindowWin32.cpp)
//   Linux:   Xlib, X11 or XWayland (UiWindowX11.cpp)
// Everything is drawn on a logical canvas that is scaled to the window. The canvas is at least the design size
// (960 x 748 for the app) and grows with the window: the scale is what fits the design size into the window, at most
// kMaxZoom times the display scale, and the canvas is the window at that scale (fitCanvas). So a window of the
// design's shape shows the design scaled; a wider or taller one (or one bigger than kMaxZoom allows) gives the views
// more room, which they use (width(), height(): wider fields and bars, more log rows, more picker columns, buttons
// kept at the right edge and the tab bar at the bottom). Text is monospaced: one character every 8 logical pixels,
// 8 logical pixels tall (the cell the layout was made for). The window cannot be made smaller than kMinZoom times the
// design size (times the display scale; less only when the screen is), nor bigger than the screen.

struct Colour {
    std::uint8_t r, g, b;
};

inline constexpr float kCharWidth = 8.0F;   // logical pixels from one character to the next
inline constexpr float kCharHeight = 8.0F;  // logical pixels of a line of text

// How far the design grows with the window (times the display scale) before the window's extra size becomes room
// on the canvas instead, and the smallest window (times the design size and the display scale).
inline constexpr float kMaxZoom = 1.5F;
inline constexpr float kMinZoom = 0.75F;

// Logical canvas -> window pixels: x * scale + offsetX; the canvas is width x height logical pixels.
struct Letterbox {
    float scale = 1.0F;
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

// The canvas for a window: the scale fits the design (logicalWidth x logicalHeight) into the window, at most
// `maxScale`; the canvas is the whole window at that scale, so it is never smaller than the design in either
// direction and there are no bars. A window smaller than the design (only when the screen is) shows it scaled down.
[[nodiscard]] inline Letterbox fitCanvas(const int windowWidth, const int windowHeight, const int logicalWidth,
                                         const int logicalHeight, const float maxScale = 1.0e6F) {
    Letterbox box;
    box.width = static_cast<float>(std::max(logicalWidth, 1));
    box.height = static_cast<float>(std::max(logicalHeight, 1));
    if (windowWidth <= 0 || windowHeight <= 0 || logicalWidth <= 0 || logicalHeight <= 0) {
        return box;
    }
    box.scale = std::min(static_cast<float>(windowWidth) / static_cast<float>(logicalWidth),
                         static_cast<float>(windowHeight) / static_cast<float>(logicalHeight));
    if (maxScale > 0.0F) box.scale = std::min(box.scale, maxScale);
    box.width = std::max(static_cast<float>(logicalWidth), std::floor(static_cast<float>(windowWidth) / box.scale));
    box.height = std::max(static_cast<float>(logicalHeight), std::floor(static_cast<float>(windowHeight) / box.scale));
    return box;
}

// A left mouse click, in logical canvas pixels (the drawing origin not applied).
struct Click {
    float x = 0.0F;
    float y = 0.0F;
};

inline constexpr char kEscape = '\x1b';
// Navigation keys, sent in WindowInput::keys like letters.
inline constexpr char kPageUp = '\x01';
inline constexpr char kPageDown = '\x02';
inline constexpr char kHome = '\x03';
inline constexpr char kEnd = '\x04';
inline constexpr char kUp = '\x05';
inline constexpr char kDown = '\x06';
inline constexpr char kLeft = '\x0e';
inline constexpr char kRight = '\x0f';
// Editing keys, sent in WindowInput::text.
inline constexpr char kBackspace = '\b';
inline constexpr char kEnter = '\r';

// What happened since the last pump().
struct WindowInput {
    bool close = false;         // the window's close button
    std::string keys;           // keys pressed (not repeats): letters in lower case ("f", "u"), kEscape
    // Text typed, for a text field: printable ASCII as typed (case, digits, punctuation, repeats included), plus
    // kBackspace and kEnter. The same key presses also arrive in `keys`; a view that is editing text ignores those.
    std::string text;
    std::vector<Click> clicks;  // left mouse button presses
    int wheel = 0;              // mouse wheel notches since the last pump(): > 0 away from the user (scroll up)
    int hwheel = 0;             // sideways wheel (or Shift + wheel) notches: > 0 to the right
    // Where the mouse is (logical canvas pixels) and whether the left button is held: for dragging (scroll bars).
    // While the button is held the position keeps coming even outside the window.
    Click pointer;
    bool leftDown = false;
    // Whether the mouse is over the window (hover tooltips go when it leaves). Moves arrive with no button held too.
    bool pointerInside = false;
};

class UiWindow {
public:
    virtual ~UiWindow() = default;

    // Handles the system's window messages; call it often (every frame).
    virtual WindowInput pump() = 0;

    // False while minimised or hidden: drawing is skipped then (a hidden window can block, e.g. while a game
    // switches display mode).
    [[nodiscard]] virtual bool visible() const = 0;

    // The canvas now, in logical pixels: at least the design size, more when the window has room (fitCanvas). Set by
    // pump() and begin(), so input and drawing in one frame see the same size.
    [[nodiscard]] float width() const { return canvasWidth_; }
    [[nodiscard]] float height() const { return canvasHeight_; }

    // Where (0, 0) of the draw calls below is on the canvas, so a panel can be drawn anywhere unchanged.
    void setOrigin(const float x, const float y) {
        originX_ = x;
        originY_ = y;
    }

    // One frame: begin() clears the canvas, the draw calls follow, present() shows it.
    virtual void begin(Colour background) = 0;
    virtual void fill(float x, float y, float w, float h, Colour colour) = 0;
    virtual void outline(float x, float y, float w, float h, Colour colour) = 0;   // 1 logical pixel wide
    virtual void text(float x, float y, std::string_view value, Colour colour) = 0; // top-left of the first cell
    virtual void present() = 0;

    // Filled shapes (the Mapping tab's pictures), drawn as rows of fill() one window pixel tall (pixelSize()), so
    // every window, the tests' recording ones included, draws them with what it already has (core ShapeSpans.hpp).
    // A polygon is filled even-odd; a ring is the circle of radius `outer` less the one of radius `inner`.
    void fillPolygon(const std::span<const ShapePoint> points, const Colour colour) {
        polygonSpans(points, pixelSize(), spans_);
        fillSpans(colour);
    }
    void fillEllipse(const float cx, const float cy, const float rx, const float ry, const Colour colour) {
        ellipseSpans(cx, cy, rx, ry, pixelSize(), spans_);
        fillSpans(colour);
    }
    void fillRing(const float cx, const float cy, const float outer, const float inner, const Colour colour) {
        ringSpans(cx, cy, outer, inner, pixelSize(), spans_);
        fillSpans(colour);
    }
    // How many logical pixels one window pixel is (1 / the scale): the rows the shapes above are drawn in.
    [[nodiscard]] virtual float pixelSize() const { return 1.0F; }

    // Opens a folder in the system's file manager. False and `error` set if that failed.
    virtual bool openFolder(const std::filesystem::path& folder, std::string& error) = 0;

    // Lets the user choose a folder in the system's own folder window (modal, over this window), starting in `folder`
    // when it is set. True and `folder` set when one was chosen; false when cancelled (`error` empty) or when there is
    // no such window here (`error` says so; X11 has none).
    virtual bool pickFolder(const std::string& title, std::filesystem::path& folder, std::string& error) = 0;

    // A system notification (Windows: the notification area's balloon, shown as a toast and kept in the notification
    // centre), seen even while a game is in front. False when there is none here (X11) or it failed: the caller shows
    // it in the window too either way.
    virtual bool notify(const std::string& /*title*/, const std::string& /*text*/) { return false; }

    // For the log: "Win32 GDI", "X11 (font ...)".
    [[nodiscard]] virtual std::string describe() const = 0;

    // Windows: called when the user logs off or Windows shuts down (WM_ENDSESSION), from pump(). The process ends
    // when it returns, so it must stop the session and wait for its clean-up (the real wheel shown again). No
    // console handler reaches a window program. Elsewhere it is never called.
    void onEndSession(std::function<void()> handler) { endSession_ = std::move(handler); }
    // Windows: called when logoff or shutdown asks whether it may go ahead (WM_QUERYENDSESSION), from pump(); the
    // answer is always yes. For a window pumped by the session's own thread (--ui), which cannot wait inside
    // onEndSession: it stops pumping from here on, so the session cleans up before WM_ENDSESSION is delivered.
    void onQueryEndSession(std::function<void()> handler) { queryEndSession_ = std::move(handler); }

protected:
    void fillSpans(const Colour colour) {
        const float row = pixelSize();
        for (const ShapeSpan& span : spans_) fill(span.x, span.y, span.w, row, colour);
    }

    std::vector<ShapeSpan> spans_;   // the last shape's rows (kept, so drawing a shape allocates nothing)
    std::function<void()> endSession_;
    std::function<void()> queryEndSession_;
    float originX_ = 0.0F;
    float originY_ = 0.0F;
    float canvasWidth_ = 960.0F;
    float canvasHeight_ = 748.0F;
};

// The window for this system, `windowWidth` x `windowHeight` (at 100 % display scale; made smaller if the
// screen is), resizable from kMinZoom times the design (`logicalWidth` x `logicalHeight`) to the screen. Nullptr,
// with `error` set, if it cannot be opened (no display, ...).
[[nodiscard]] std::unique_ptr<UiWindow> openWindow(const std::string& title, int logicalWidth, int logicalHeight,
                                                   int windowWidth, int windowHeight, std::string& error);

}  // namespace wheelbri::service::ui
