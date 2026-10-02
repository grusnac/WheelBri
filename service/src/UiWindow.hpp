#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wheelemu::service::ui {

// The `--ui` window's drawing surface, implemented with each system's own tools:
//   Windows: Win32 + GDI (UiWindowWin32.cpp)
//   Linux:   Xlib, X11 or XWayland (UiWindowX11.cpp)
// Everything is drawn on a fixed logical canvas that is scaled to the window, centred (letterboxed), so the
// layout code never deals with window sizes. Text is monospaced: one character every 8 logical pixels,
// 8 logical pixels tall (the cell the layout was made for).

struct Colour {
    std::uint8_t r, g, b;
};

inline constexpr float kCharWidth = 8.0F;   // logical pixels from one character to the next
inline constexpr float kCharHeight = 8.0F;  // logical pixels of a line of text

// Logical canvas -> window pixels: x * scale + offsetX.
struct Letterbox {
    float scale = 1.0F;
    float offsetX = 0.0F;
    float offsetY = 0.0F;
};

[[nodiscard]] inline Letterbox fitCanvas(const int windowWidth, const int windowHeight, const int logicalWidth,
                                         const int logicalHeight) {
    Letterbox box;
    if (windowWidth <= 0 || windowHeight <= 0 || logicalWidth <= 0 || logicalHeight <= 0) {
        return box;
    }
    box.scale = std::min(static_cast<float>(windowWidth) / static_cast<float>(logicalWidth),
                         static_cast<float>(windowHeight) / static_cast<float>(logicalHeight));
    box.offsetX = std::floor((static_cast<float>(windowWidth) - static_cast<float>(logicalWidth) * box.scale) * 0.5F);
    box.offsetY =
        std::floor((static_cast<float>(windowHeight) - static_cast<float>(logicalHeight) * box.scale) * 0.5F);
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

// What happened since the last pump().
struct WindowInput {
    bool close = false;         // the window's close button
    std::string keys;           // keys pressed (not repeats): letters in lower case ("f", "u"), kEscape
    std::vector<Click> clicks;  // left mouse button presses
    int wheel = 0;              // mouse wheel notches since the last pump(): > 0 away from the user (scroll up)
};

class UiWindow {
public:
    virtual ~UiWindow() = default;

    // Handles the system's window messages; call it often (every frame).
    virtual WindowInput pump() = 0;

    // False while minimised or hidden: drawing is skipped then (a hidden window can block, e.g. while a game
    // switches display mode).
    [[nodiscard]] virtual bool visible() const = 0;

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

    // Opens a folder in the system's file manager. False and `error` set if that failed.
    virtual bool openFolder(const std::filesystem::path& folder, std::string& error) = 0;

    // For the log: "Win32 GDI", "X11 (font ...)".
    [[nodiscard]] virtual std::string describe() const = 0;

protected:
    float originX_ = 0.0F;
    float originY_ = 0.0F;
};

// The window for this system, `windowWidth` x `windowHeight` (at 100 % display scale; made smaller if the
// screen is). Nullptr, with `error` set, if it cannot be opened (no display, ...).
[[nodiscard]] std::unique_ptr<UiWindow> openWindow(const std::string& title, int logicalWidth, int logicalHeight,
                                                   int windowWidth, int windowHeight, std::string& error);

}  // namespace wheelemu::service::ui
