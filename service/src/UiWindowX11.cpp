// The `--ui` window on Linux: a plain Xlib window. Works on X11 desktops and on Wayland ones through
// XWayland (KDE on the Steam Deck included); needs only libX11, which every Linux desktop has.
//
// Each frame is drawn into a pixmap the size of the window (the logical canvas scaled and centred), then
// copied to the window in one go, so it never flickers. Text uses the X server's own fixed-width fonts
// ("misc-fixed", always there), the size closest to the scaled 8-pixel grid.

#include "UiWindow.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <spawn.h>
#include <sys/wait.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace wheelemu::service::ui {

namespace {

// Position of the lowest set bit of a colour mask, and how many bits it has.
struct Channel {
    int shift = 0;
    int bits = 0;
};

Channel channel(unsigned long mask) {
    Channel c;
    if (mask == 0) return c;
    while ((mask & 1UL) == 0) {
        mask >>= 1U;
        ++c.shift;
    }
    while ((mask & 1UL) != 0) {
        mask >>= 1U;
        ++c.bits;
    }
    return c;
}

unsigned long place(const std::uint8_t value, const Channel c) {
    const unsigned long scaled = c.bits >= 8 ? static_cast<unsigned long>(value) << (c.bits - 8)
                                             : static_cast<unsigned long>(value) >> (8 - c.bits);
    return scaled << c.shift;
}

// One of the server's fonts by its XLFD name: pixel size (field 7) and average width (field 12, tenths).
struct FontChoice {
    std::string name;
    int pixels = 0;
    int width = 0;   // pixels
};

bool parseXlfd(const char* name, FontChoice& out) {
    // -foundry-family-weight-slant-setwidth-addstyle-PIXELS-points-resx-resy-spacing-AVERAGE-registry-encoding
    std::array<std::string, 15> fields;
    std::size_t field = 0;
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == '-') {
            if (++field >= fields.size()) return false;
        } else if (field > 0) {
            fields[field - 1] += *p;
        }
    }
    if (field != 14) return false;
    out.name = name;
    out.pixels = std::atoi(fields[6].c_str());
    out.width = (std::atoi(fields[11].c_str()) + 5) / 10;
    return out.pixels > 0 && out.width > 0;
}

class X11Window final : public UiWindow {
public:
    X11Window(Display* display, const int logicalWidth, const int logicalHeight)
        : display_(display), logicalWidth_(logicalWidth), logicalHeight_(logicalHeight) {}

    ~X11Window() override {
        if (font_ != nullptr) XFreeFont(display_, font_);
        if (pixmap_ != 0) XFreePixmap(display_, pixmap_);
        if (gc_ != nullptr) XFreeGC(display_, gc_);
        if (window_ != 0) XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
    }

    X11Window(const X11Window&) = delete;
    X11Window& operator=(const X11Window&) = delete;

    bool create(const std::string& title, int width, int height, std::string& error) {
        const int screen = DefaultScreen(display_);
        Visual* const visual = DefaultVisual(display_, screen);
        if (visual->c_class != TrueColor && visual->c_class != DirectColor) {
            error = "the X display has no true-colour visual";
            return false;
        }
        red_ = channel(visual->red_mask);
        green_ = channel(visual->green_mask);
        blue_ = channel(visual->blue_mask);
        depth_ = DefaultDepth(display_, screen);

        // Smaller (same shape) if the screen is: leave room for panels and the title bar.
        const double fit = std::min({1.0, (DisplayWidth(display_, screen) - 40.0) / width,
                                     (DisplayHeight(display_, screen) - 90.0) / height});
        width = std::max(160, static_cast<int>(width * fit));
        height = std::max(160, static_cast<int>(height * fit));

        window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen), 0, 0, static_cast<unsigned>(width),
                                      static_cast<unsigned>(height), 0, BlackPixel(display_, screen),
                                      BlackPixel(display_, screen));
        if (window_ == 0) {
            error = "XCreateSimpleWindow failed";
            return false;
        }
        XSetWindowBackgroundPixmap(display_, window_, None);   // no flash of the background on resize
        XStoreName(display_, window_, title.c_str());
        const Atom utf8 = XInternAtom(display_, "UTF8_STRING", False);
        XChangeProperty(display_, window_, XInternAtom(display_, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(title.c_str()), static_cast<int>(title.size()));
        XClassHint classHint{const_cast<char*>("wheelemu-service"), const_cast<char*>("wheelemu")};
        XSetClassHint(display_, window_, &classHint);
        deleteWindow_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &deleteWindow_, 1);
        XSelectInput(display_, window_,
                     ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask | VisibilityChangeMask);
        // Held keys repeat as presses only (no fake releases), so a repeat is recognisable.
        Bool supported = False;
        XkbSetDetectableAutoRepeat(display_, True, &supported);

        gc_ = XCreateGC(display_, window_, 0, nullptr);
        width_ = width;
        height_ = height;
        box_ = fitCanvas(width_, height_, logicalWidth_, logicalHeight_);
        updateFont();
        XMapWindow(display_, window_);
        XFlush(display_);
        return true;
    }

    WindowInput pump() override {
        while (XPending(display_) > 0) {
            XEvent event;
            XNextEvent(display_, &event);
            switch (event.type) {
                case ClientMessage:
                    if (static_cast<Atom>(event.xclient.data.l[0]) == deleteWindow_) closed_ = true;
                    break;
                case KeyPress: {
                    const unsigned code = event.xkey.keycode & 0xFFU;
                    const bool repeat = keyDown_[code];
                    keyDown_[code] = true;
                    const KeySym sym = XLookupKeysym(&event.xkey, 0);
                    if (sym == XK_Escape) {
                        closed_ = true;
                    } else if (!repeat && sym >= XK_a && sym <= XK_z) {
                        keys_ += static_cast<char>('a' + (sym - XK_a));
                    }
                    break;
                }
                case KeyRelease:
                    keyDown_[event.xkey.keycode & 0xFFU] = false;
                    break;
                case ConfigureNotify:
                    width_ = event.xconfigure.width;
                    height_ = event.xconfigure.height;
                    break;
                case MapNotify:
                    mapped_ = true;
                    break;
                case UnmapNotify:   // minimised
                    mapped_ = false;
                    break;
                case VisibilityNotify:
                    obscured_ = event.xvisibility.state == VisibilityFullyObscured;
                    break;
                case Expose:
                    if (event.xexpose.count == 0) copyToWindow();
                    break;
                default:
                    break;
            }
        }
        WindowInput input;
        input.close = closed_;
        input.keys.swap(keys_);
        return input;
    }

    [[nodiscard]] bool visible() const override { return mapped_ && !obscured_; }

    void begin(const Colour background) override {
        const int width = std::max(1, width_);
        const int height = std::max(1, height_);
        if (pixmap_ == 0 || width != pixmapWidth_ || height != pixmapHeight_) {
            if (pixmap_ != 0) XFreePixmap(display_, pixmap_);
            pixmap_ = XCreatePixmap(display_, window_, static_cast<unsigned>(width), static_cast<unsigned>(height),
                                    static_cast<unsigned>(depth_));
            pixmapWidth_ = width;
            pixmapHeight_ = height;
        }
        box_ = fitCanvas(width, height, logicalWidth_, logicalHeight_);
        updateFont();
        XSetForeground(display_, gc_, pixel(background));
        XFillRectangle(display_, pixmap_, gc_, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height));
    }

    void fill(const float x, const float y, const float w, const float h, const Colour colour) override {
        if (w <= 0.0F || h <= 0.0F) {
            return;
        }
        const int left = px(x);
        const int top = py(y);
        const int width = std::max(1, px(x + w) - left);   // a thin line never disappears
        const int height = std::max(1, py(y + h) - top);
        XSetForeground(display_, gc_, pixel(colour));
        XFillRectangle(display_, pixmap_, gc_, left, top, static_cast<unsigned>(width),
                       static_cast<unsigned>(height));
    }

    void outline(const float x, const float y, const float w, const float h, const Colour colour) override {
        const float line = 1.0F / box_.scale * static_cast<float>(std::max(1L, std::lround(box_.scale)));
        fill(x, y, w, line, colour);
        fill(x, y + h - line, w, line, colour);
        fill(x, y, line, h, colour);
        fill(x + w - line, y, line, h, colour);
    }

    void text(const float x, const float y, const std::string_view value, const Colour colour) override {
        if (font_ == nullptr || value.empty()) {
            return;
        }
        XSetForeground(display_, gc_, pixel(colour));
        // Every character centred in its cell of the 8-pixel grid, exactly where the layout put it; the
        // baseline 8 pixels down, so the (bigger) letters are centred on the 8-pixel line.
        const int glyphWidth = font_->max_bounds.width;
        const int baseline = py(y + 8.0F);
        for (std::size_t i = 0; i < value.size(); ++i) {
            char c = value[i];
            if (c == ' ') continue;
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
            const float left = x + static_cast<float>(i) * kCharWidth;
            const int cell = px(left + kCharWidth) - px(left);
            XDrawString(display_, pixmap_, gc_, px(left) + (cell - glyphWidth) / 2, baseline, &c, 1);
        }
    }

    void present() override {
        copyToWindow();
        XFlush(display_);
    }

    bool openFolder(const std::filesystem::path& folder, std::string& error) override {
        const std::string path = folder.string();
        char program[] = "xdg-open";
        std::vector<char> argument(path.begin(), path.end());
        argument.push_back('\0');
        char* const argv[] = {program, argument.data(), nullptr};
        pid_t pid = 0;
        const int result = posix_spawnp(&pid, program, nullptr, nullptr, argv, environ);
        if (result != 0) {
            error = std::string("xdg-open: ") + std::strerror(result);
            return false;
        }
        std::thread([pid] {   // collect it when it ends, so no zombie stays behind
            int status = 0;
            waitpid(pid, &status, 0);
        }).detach();
        return true;
    }

    [[nodiscard]] std::string describe() const override {
        return "X11, font " + (fontName_.empty() ? std::string("none") : fontName_);
    }

private:
    [[nodiscard]] int px(const float x) const { return static_cast<int>(std::lround(box_.offsetX + x * box_.scale)); }
    [[nodiscard]] int py(const float y) const { return static_cast<int>(std::lround(box_.offsetY + y * box_.scale)); }

    [[nodiscard]] unsigned long pixel(const Colour c) const {
        return place(c.r, red_) | place(c.g, green_) | place(c.b, blue_);
    }

    void copyToWindow() {
        if (pixmap_ != 0) {
            XCopyArea(display_, pixmap_, window_, gc_, 0, 0, static_cast<unsigned>(pixmapWidth_),
                      static_cast<unsigned>(pixmapHeight_), 0, 0);
        }
    }

    // The biggest fixed-width font whose characters fit the scaled 8 x 14 cell (lines are 11..16 apart, and
    // a font's pixel size includes room above and below its letters).
    void updateFont() {
        if (font_ != nullptr && std::abs(fontScale_ - box_.scale) < 0.001F) {
            return;
        }
        fontScale_ = box_.scale;
        const int maxWidth = std::max(5, static_cast<int>(std::lround(kCharWidth * box_.scale)));
        const int maxPixels = std::max(8, static_cast<int>(std::lround(14.0F * box_.scale)));
        FontChoice best;
        FontChoice smallest;
        for (const char* pattern : {"-misc-fixed-medium-r-normal--*-*-*-*-c-*-iso10646-1",
                                    "-misc-fixed-medium-r-normal--*-*-*-*-c-*-iso8859-1",
                                    "-*-*-medium-r-normal--*-*-*-*-[cm]-*-iso8859-1"}) {
            int count = 0;
            char** const names = XListFonts(display_, pattern, 500, &count);
            for (int i = 0; i < count; ++i) {
                FontChoice candidate;
                if (!parseXlfd(names[i], candidate)) continue;
                if (smallest.name.empty() || candidate.pixels < smallest.pixels) smallest = candidate;
                if (candidate.width <= maxWidth && candidate.pixels <= maxPixels &&
                    (candidate.pixels > best.pixels ||
                     (candidate.pixels == best.pixels && candidate.width > best.width))) {
                    best = candidate;
                }
            }
            if (names != nullptr) XFreeFontNames(names);
            if (!best.name.empty()) break;
        }
        if (best.name.empty()) best = smallest;

        XFontStruct* font = best.name.empty() ? nullptr : XLoadQueryFont(display_, best.name.c_str());
        std::string name = best.name;
        if (font == nullptr) {
            font = XLoadQueryFont(display_, "fixed");   // built into every X server
            name = "fixed";
        }
        if (font == nullptr) {
            return;   // keep the previous one
        }
        if (font_ != nullptr) XFreeFont(display_, font_);
        font_ = font;
        fontName_ = name;
        XSetFont(display_, gc_, font_->fid);
    }

    Display* display_;
    int logicalWidth_;
    int logicalHeight_;
    Window window_ = 0;
    GC gc_ = nullptr;
    Pixmap pixmap_ = 0;
    int pixmapWidth_ = 0;
    int pixmapHeight_ = 0;
    int width_ = 0;
    int height_ = 0;
    int depth_ = 24;
    Channel red_;
    Channel green_;
    Channel blue_;
    Atom deleteWindow_ = 0;
    XFontStruct* font_ = nullptr;
    std::string fontName_;
    float fontScale_ = -1.0F;
    Letterbox box_;
    std::array<bool, 256> keyDown_{};
    bool mapped_ = false;
    bool obscured_ = false;
    bool closed_ = false;
    std::string keys_;
};

}  // namespace

std::unique_ptr<UiWindow> openWindow(const std::string& title, const int logicalWidth, const int logicalHeight,
                                     const int windowWidth, const int windowHeight, std::string& error) {
    Display* const display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        const char* const name = std::getenv("DISPLAY");
        error = name == nullptr || *name == '\0'
                    ? std::string("no X display (DISPLAY is not set; on Wayland, XWayland is needed)")
                    : std::string("cannot connect to the X display ") + name;
        return nullptr;
    }
    auto window = std::make_unique<X11Window>(display, logicalWidth, logicalHeight);
    if (!window->create(title, windowWidth, windowHeight, error)) {
        return nullptr;
    }
    return window;
}

}  // namespace wheelemu::service::ui
