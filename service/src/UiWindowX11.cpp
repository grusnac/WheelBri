// The `--ui` window on Linux: a plain Xlib window. Works on X11 desktops and on Wayland ones through
// XWayland (KDE on the Steam Deck included); needs only libX11, which every Linux desktop has.
//
// Each frame is drawn into a pixmap the size of the window (the logical canvas scaled and centred), then
// copied to the window in one go, so it never flickers. Text uses the X server's own fixed-width fonts
// ("misc-fixed", always there), the size closest to the scaled 8-pixel grid.

#include "UiWindow.hpp"

#include "wheelbri/Logging.hpp"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace wheelbri::service::ui {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.ui");
    return logger;
}

// Runs `argv` (a full path first) and collects what it prints, waiting for it to end. Steam's preloads and
// runtime libraries are left out of its environment (the app may have been started from Steam).
bool runForOutput(const std::vector<std::string>& argv, std::string& output, int& exitCode, std::string& error) {
    std::vector<char*> args;
    std::vector<std::string> copies(argv);
    for (std::string& arg : copies) args.push_back(arg.data());
    args.push_back(nullptr);
    std::vector<char*> env;
    for (char** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0 && std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) env.push_back(*e);
    }
    env.push_back(nullptr);
    int pipeFds[2] = {-1, -1};
    if (pipe2(pipeFds, O_CLOEXEC) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    pid_t pid = 0;
    const int result = posix_spawn(&pid, args.front(), &actions, nullptr, args.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (result != 0) {
        close(pipeFds[0]);
        error = argv.front() + ": " + std::strerror(result);
        return false;
    }
    std::array<char, 1024> buffer{};
    for (;;) {
        const ssize_t got = read(pipeFds[0], buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(got));
    }
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

// The X server went away (logout, XWayland restarted): every Xlib call on the display is off from then on, and the
// window reports itself closed so the session stops cleanly.
std::atomic<bool> gDisplayLost{false};

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
        if (gDisplayLost) return;   // the connection is gone: nothing to free on the server, and no call may touch it
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
        XClassHint classHint{const_cast<char*>("wheelbri-service"), const_cast<char*>("wheelbri")};
        XSetClassHint(display_, window_, &classHint);
        deleteWindow_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &deleteWindow_, 1);
        XSelectInput(display_, window_,
                     ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                         Button1MotionMask | StructureNotifyMask |
                         VisibilityChangeMask);
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
        if (gDisplayLost) {
            WindowInput input;
            input.close = true;
            return input;
        }
        while (!gDisplayLost && XPending(display_) > 0) {
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
                    // Text for a text field: the character with Shift and the layout applied (repeats too).
                    if (sym == XK_BackSpace) {
                        text_ += kBackspace;
                    } else if (sym == XK_Return || sym == XK_KP_Enter) {
                        text_ += kEnter;
                    } else {
                        char typed[8] = {};
                        KeySym typedSym = 0;
                        const int count = XLookupString(&event.xkey, typed, sizeof(typed), &typedSym, nullptr);
                        if (count == 1 && typed[0] >= 32 && typed[0] < 127) text_ += typed[0];
                    }
                    if (sym == XK_Escape) {
                        if (!repeat) keys_ += kEscape;
                    } else if (!repeat && sym >= XK_a && sym <= XK_z) {
                        keys_ += static_cast<char>('a' + (sym - XK_a));
                    } else if (sym == XK_Prior) {
                        keys_ += kPageUp;
                    } else if (sym == XK_Next) {
                        keys_ += kPageDown;
                    } else if (sym == XK_Home) {
                        keys_ += kHome;
                    } else if (sym == XK_End) {
                        keys_ += kEnd;
                    } else if (sym == XK_Up) {
                        keys_ += kUp;
                    } else if (sym == XK_Down) {
                        keys_ += kDown;
                    } else if (sym == XK_Left) {
                        keys_ += kLeft;
                    } else if (sym == XK_Right) {
                        keys_ += kRight;
                    }
                    break;
                }
                case ButtonPress: {
                    const bool shift = (event.xbutton.state & ShiftMask) != 0;   // Shift + wheel: sideways
                    if (event.xbutton.button == Button1) {
                        pointer_ = toCanvas(event.xbutton.x, event.xbutton.y);
                        clicks_.push_back(pointer_);
                        leftDown_ = true;   // X grabs the pointer for the window while a button is held
                    } else if (event.xbutton.button == Button4) {   // wheel up
                        if (shift) --hwheel_; else ++wheel_;
                    } else if (event.xbutton.button == Button5) {   // wheel down
                        if (shift) ++hwheel_; else --wheel_;
                    } else if (event.xbutton.button == 6) {   // wheel left
                        --hwheel_;
                    } else if (event.xbutton.button == 7) {   // wheel right
                        ++hwheel_;
                    }
                    break;
                }
                case ButtonRelease:
                    if (event.xbutton.button == Button1) {
                        pointer_ = toCanvas(event.xbutton.x, event.xbutton.y);
                        leftDown_ = false;
                    }
                    break;
                case MotionNotify:
                    pointer_ = toCanvas(event.xmotion.x, event.xmotion.y);
                    break;
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
        input.close = closed_ || gDisplayLost;
        input.keys.swap(keys_);
        input.text.swap(text_);
        input.clicks.swap(clicks_);
        input.wheel = wheel_;
        wheel_ = 0;
        input.hwheel = hwheel_;
        hwheel_ = 0;
        input.pointer = pointer_;
        input.leftDown = leftDown_;
        return input;
    }

    [[nodiscard]] bool visible() const override { return mapped_ && !obscured_; }

    void begin(const Colour background) override {
        if (gDisplayLost) return;
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
        if (w <= 0.0F || h <= 0.0F || gDisplayLost || pixmap_ == 0) {
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
        if (font_ == nullptr || value.empty() || gDisplayLost || pixmap_ == 0) {
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
        if (gDisplayLost) return;
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

    // The desktop's own folder window: KDE's kdialog (the Steam Deck's Desktop Mode), else GNOME's zenity, by full
    // path (never PATH). It blocks until it closes, as the Windows one does.
    bool pickFolder(const std::string& title, std::filesystem::path& folder, std::string& error) override {
        const std::string start = folder.empty() ? std::string(std::getenv("HOME") != nullptr ? std::getenv("HOME") : "/")
                                                 : folder.string();
        const std::vector<std::vector<std::string>> dialogs = {
            {"/usr/bin/kdialog", "--title", title, "--getexistingdirectory", start},
            {"/usr/bin/zenity", "--file-selection", "--directory", "--title=" + title, "--filename=" + start + "/"},
        };
        for (const std::vector<std::string>& dialog : dialogs) {
            if (access(dialog[0].c_str(), X_OK) != 0) continue;
            std::string chosen;
            int exitCode = -1;
            if (!runForOutput(dialog, chosen, exitCode, error)) return false;
            while (!chosen.empty() && (chosen.back() == '\n' || chosen.back() == '\r')) chosen.pop_back();
            if (exitCode != 0 || chosen.empty()) return false;   // cancelled
            folder = chosen;
            return true;
        }
        error = "no folder window here (kdialog or zenity): put the shim in with scripts/linux/wheelbri.sh --shim "
                "(see docs/linux.md)";
        return false;
    }

    [[nodiscard]] std::string describe() const override {
        return "X11, font " + (fontName_.empty() ? std::string("none") : fontName_);
    }

private:
    [[nodiscard]] int px(const float x) const {
        return static_cast<int>(std::lround(box_.offsetX + (x + originX_) * box_.scale));
    }
    [[nodiscard]] int py(const float y) const {
        return static_cast<int>(std::lround(box_.offsetY + (y + originY_) * box_.scale));
    }
    // Window pixels -> logical canvas pixels.
    [[nodiscard]] Click toCanvas(const int x, const int y) const {
        const float scale = box_.scale > 0.0F ? box_.scale : 1.0F;
        return {(static_cast<float>(x) - box_.offsetX) / scale, (static_cast<float>(y) - box_.offsetY) / scale};
    }

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
    std::string text_;
    std::vector<Click> clicks_;
    int wheel_ = 0;
    int hwheel_ = 0;
    Click pointer_;
    bool leftDown_ = false;
};

}  // namespace

std::unique_ptr<UiWindow> openWindow(const std::string& title, const int logicalWidth, const int logicalHeight,
                                     const int windowWidth, const int windowHeight, std::string& error) {
    // Xlib's default error handler ends the process (exit) on any protocol error, e.g. BadAlloc for a pixmap:
    // the session would stop without its clean-up. Log it instead and carry on.
    XSetErrorHandler([](Display* display, XErrorEvent* event) -> int {
        // The same error can repeat every frame (60 a second): a line every 10 s at most.
        static logging::RateLimiter limiter(std::chrono::seconds(10));
        std::uint64_t suppressed = 0;
        if (!limiter.allow(suppressed)) return 0;
        char text[128] = {};
        XGetErrorText(display, event->error_code, text, sizeof(text));
        WE_LOG_ERROR(log(), "X11 error {} ({}), request {} ({} similar suppressed)", static_cast<int>(event->error_code),
                     text, static_cast<int>(event->request_code), suppressed);
        return 0;
    });
    // The connection itself broke. Xlib then ends the process unless an exit handler (libX11 1.7+) returns.
    XSetIOErrorHandler([](Display*) -> int {
        WE_LOG_CRITICAL(log(), "the X display went away");
        gDisplayLost = true;
        return 0;
    });
    Display* const display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        const char* const name = std::getenv("DISPLAY");
        error = name == nullptr || *name == '\0'
                    ? std::string("no X display (DISPLAY is not set; on Wayland, XWayland is needed)")
                    : std::string("cannot connect to the X display ") + name;
        return nullptr;
    }
#ifdef WE_HAVE_XIO_EXIT_HANDLER
    // Returning from here keeps the process alive: the window says closed and the session stops and cleans up.
    XSetIOErrorExitHandler(
        display, [](Display*, void*) { gDisplayLost = true; }, nullptr);
#endif
    auto window = std::make_unique<X11Window>(display, logicalWidth, logicalHeight);
    if (!window->create(title, windowWidth, windowHeight, error)) {
        return nullptr;
    }
    return window;
}

}  // namespace wheelbri::service::ui
