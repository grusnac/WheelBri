// The Linux window (service/src/UiWindowX11.cpp) on a real X server: each test starts its own Xvfb (skipped when
// Xvfb is not installed), opens the window, sends it what a desktop sends (keys, buttons, the wheel, moves, resizes,
// close, unmap) from a second connection and checks what pump() makes of it; draws a frame; opens a folder through
// a stand-in xdg-open; and survives the X server going away.

// GoogleTest before Xlib: Xlib defines macros (None, Bool, Status) that its headers would trip over.
#include <gtest/gtest.h>

#include "UiWindow.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace wheelbri::service::ui {
namespace {

// An Xvfb of the test's own on a free display number; DISPLAY points at it while it runs.
class Xvfb {
public:
    explicit Xvfb(const std::string& screen = "1280x1024x24") {
        if (access("/usr/bin/Xvfb", X_OK) != 0) return;
        // Xvfb picks a free display number itself and writes it to the pipe once it is ready (-displayfd).
        int fds[2] = {-1, -1};
        if (pipe(fds) != 0) return;
        std::vector<std::string> args = {"/usr/bin/Xvfb", "-displayfd", std::to_string(fds[1]), "-screen", "0",
                                          screen, "-nolisten", "tcp"};
        std::vector<char*> argv;
        for (std::string& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", 1, 0);
        const int result = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        if (result != 0) {
            pid_ = 0;
            close(fds[0]);
            return;
        }
        std::string number;
        char c = 0;
        while (read(fds[0], &c, 1) == 1 && c != '\n') number += c;
        close(fds[0]);
        if (number.empty()) {
            stop();
            return;
        }
        display_ = ":" + number;
        const char* const old = std::getenv("DISPLAY");
        if (old != nullptr) oldDisplay_ = old;
        setenv("DISPLAY", display_.c_str(), 1);
        up_ = true;
    }
    ~Xvfb() {
        stop();
        if (oldDisplay_) {
            setenv("DISPLAY", oldDisplay_->c_str(), 1);
        } else {
            unsetenv("DISPLAY");
        }
    }
    Xvfb(const Xvfb&) = delete;
    Xvfb& operator=(const Xvfb&) = delete;

    [[nodiscard]] bool up() const { return up_; }
    [[nodiscard]] const std::string& display() const { return display_; }
    void stop() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = 0;
        }
        up_ = false;
    }

private:
    std::string display_;
    pid_t pid_ = 0;
    bool up_ = false;
    std::optional<std::string> oldDisplay_;
};

// A second connection to the same server: finds the app's window by its title and sends it events.
class Desktop {
public:
    explicit Desktop(const std::string& display) : display_(XOpenDisplay(display.c_str())) {}
    ~Desktop() {
        if (display_ != nullptr) XCloseDisplay(display_);
    }
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;

    [[nodiscard]] Window find(const std::string& title) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            Window root = 0;
            Window parent = 0;
            Window* children = nullptr;
            unsigned count = 0;
            XQueryTree(display_, DefaultRootWindow(display_), &root, &parent, &children, &count);
            for (unsigned i = 0; i < count; ++i) {
                char* name = nullptr;
                if (XFetchName(display_, children[i], &name) != 0 && name != nullptr) {
                    const bool match = title == name;
                    XFree(name);
                    if (match) {
                        const Window found = children[i];
                        XFree(children);
                        return found;
                    }
                }
            }
            if (children != nullptr) XFree(children);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }
    void send(const Window window, XEvent event) {
        event.xany.window = window;
        event.xany.display = display_;
        XSendEvent(display_, window, False, 0, &event);
        XFlush(display_);
    }
    void key(const Window window, const KeySym sym, const bool press = true, const unsigned state = 0) {
        XEvent event{};
        event.xkey.type = press ? KeyPress : KeyRelease;
        event.xkey.keycode = XKeysymToKeycode(display_, sym);
        event.xkey.state = state;
        event.xkey.same_screen = True;
        event.xkey.root = DefaultRootWindow(display_);
        send(window, event);
    }
    void button(const Window window, const unsigned button, const bool press, const int x, const int y,
                const unsigned state = 0) {
        XEvent event{};
        event.xbutton.type = press ? ButtonPress : ButtonRelease;
        event.xbutton.button = button;
        event.xbutton.x = x;
        event.xbutton.y = y;
        event.xbutton.state = state;
        send(window, event);
    }
    void typed(const Window window, const int type, const std::function<void(XEvent&)>& fill) {
        XEvent event{};
        event.type = type;
        fill(event);
        send(window, event);
    }
    [[nodiscard]] Display* display() const { return display_; }

private:
    Display* display_;
};

// pump() until `done` says so (events take a moment to arrive), at most a second; everything seen, merged.
WindowInput pumpUntil(UiWindow& window, const std::function<bool(const WindowInput&)>& done) {
    WindowInput all;
    for (int i = 0; i < 200; ++i) {
        const WindowInput input = window.pump();
        all.close = all.close || input.close;
        all.keys += input.keys;
        all.text += input.text;
        all.clicks.insert(all.clicks.end(), input.clicks.begin(), input.clicks.end());
        all.wheel += input.wheel;
        all.hwheel += input.hwheel;
        all.pointer = input.pointer;
        all.leftDown = input.leftDown;
        all.pointerInside = input.pointerInside;
        if (done(all)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return all;
}

// Without a display (none set, or one nobody serves) the window cannot open, and says why.
TEST(UiWindowX11, NoDisplay) {
    const char* const old = std::getenv("DISPLAY");
    const std::optional<std::string> saved = old != nullptr ? std::optional<std::string>(old) : std::nullopt;
    std::string error;
    unsetenv("DISPLAY");
    EXPECT_EQ(openWindow("WheelBri", 960, 748, 1248, 972, error), nullptr);
    EXPECT_NE(error.find("DISPLAY is not set"), std::string::npos) << error;
    setenv("DISPLAY", ":399", 1);
    EXPECT_EQ(openWindow("WheelBri", 960, 748, 1248, 972, error), nullptr);
    EXPECT_NE(error.find("cannot connect to the X display :399"), std::string::npos) << error;
    if (saved) {
        setenv("DISPLAY", saved->c_str(), 1);
    } else {
        unsetenv("DISPLAY");
    }
}

// A display without a true-colour visual (8-bit) is refused.
TEST(UiWindowX11, NeedsTrueColour) {
    Xvfb server("640x480x8");
    if (!server.up()) GTEST_SKIP() << "no Xvfb";
    std::string error;
    EXPECT_EQ(openWindow("WheelBri", 960, 748, 1248, 972, error), nullptr);
    EXPECT_EQ(error, "the X display has no true-colour visual");
}

// Every event a desktop sends becomes the window's input: letters (not repeats), the named keys, typed text with
// Backspace and Enter, the left button (clicks and held), the wheel (Shift: sideways) and the sideways buttons,
// moves, entering and leaving, a resize (the canvas grows), unmapping (not visible), the close button.
TEST(UiWindowX11, EventsBecomeInput) {
    Xvfb server;
    if (!server.up()) GTEST_SKIP() << "no Xvfb";
    std::string error;
    auto window = openWindow("WheelBri events", 960, 748, 1248, 972, error);
    ASSERT_NE(window, nullptr) << error;
    EXPECT_NE(window->describe().find("X11, font "), std::string::npos);
    Desktop desktop(server.display());
    const Window id = desktop.find("WheelBri events");
    ASSERT_NE(id, 0U);
    // Mapped: visible.
    WindowInput input = pumpUntil(*window, [&window](const WindowInput&) { return window->visible(); });
    EXPECT_TRUE(window->visible());

    // Keys: a letter, the same again while held (a repeat: no key, but text), its release, the named keys.
    desktop.key(id, XK_m);
    desktop.key(id, XK_m);
    desktop.key(id, XK_m, false);
    for (const KeySym sym : {XK_Escape, XK_Prior, XK_Next, XK_Home, XK_End, XK_Up, XK_Down, XK_Left, XK_Right,
                             XK_BackSpace, XK_Return, XK_KP_Enter, XK_F1}) {
        desktop.key(id, sym);
        desktop.key(id, sym, false);
    }
    desktop.key(id, XK_Escape);   // Escape held: the repeat is not another key
    desktop.key(id, XK_Escape);
    input = pumpUntil(*window, [](const WindowInput& in) { return in.keys.size() >= 11; });
    EXPECT_EQ(input.keys, std::string("m") + kEscape + kPageUp + kPageDown + kHome + kEnd + kUp + kDown + kLeft +
                              kRight + kEscape);
    EXPECT_EQ(input.text.substr(0, 2), "mm");
    EXPECT_NE(input.text.find(kBackspace), std::string::npos);
    EXPECT_NE(input.text.find(kEnter), std::string::npos);

    // Buttons: a click (held, then released), the wheel both ways, with Shift sideways, the sideways buttons, another
    // button (ignored).
    desktop.button(id, Button1, true, 100, 100);
    input = pumpUntil(*window, [](const WindowInput& in) { return !in.clicks.empty(); });
    ASSERT_EQ(input.clicks.size(), 1U);
    EXPECT_TRUE(input.leftDown);
    desktop.button(id, Button1, false, 110, 120);
    for (const unsigned b : {Button4, Button5}) {
        desktop.button(id, b, true, 0, 0);
        desktop.button(id, b, true, 0, 0, ShiftMask);
    }
    desktop.button(id, Button4, true, 0, 0);
    desktop.button(id, 6, true, 0, 0);
    desktop.button(id, 6, true, 0, 0);
    desktop.button(id, 7, true, 0, 0);
    desktop.button(id, Button2, true, 0, 0);
    desktop.button(id, Button2, false, 0, 0);
    input = pumpUntil(*window, [](const WindowInput& in) { return in.wheel == 1 && in.hwheel == -1 && !in.leftDown; });
    EXPECT_EQ(input.wheel, 1);
    EXPECT_EQ(input.hwheel, -1);
    EXPECT_FALSE(input.leftDown);

    // The mouse: moves, enters, leaves (a grab's crossing does not count).
    desktop.typed(id, MotionNotify, [](XEvent& e) {
        e.xmotion.x = 200;
        e.xmotion.y = 150;
    });
    input = pumpUntil(*window, [](const WindowInput& in) { return in.pointerInside; });
    EXPECT_TRUE(input.pointerInside);
    desktop.typed(id, LeaveNotify, [](XEvent& e) { e.xcrossing.mode = NotifyGrab; });
    desktop.typed(id, LeaveNotify, [](XEvent& e) { e.xcrossing.mode = NotifyNormal; });
    input = pumpUntil(*window, [](const WindowInput& in) { return !in.pointerInside; });
    EXPECT_FALSE(input.pointerInside);
    desktop.typed(id, EnterNotify, [](XEvent& e) {
        e.xcrossing.x = 5;
        e.xcrossing.y = 5;
    });
    input = pumpUntil(*window, [](const WindowInput& in) { return in.pointerInside; });
    EXPECT_TRUE(input.pointerInside);

    // A bigger window: more canvas (past kMaxZoom the design stays and the room grows).
    desktop.typed(id, ConfigureNotify, [](XEvent& e) {
        e.xconfigure.width = 2400;
        e.xconfigure.height = 1400;
    });
    pumpUntil(*window, [&window](const WindowInput&) { return window->width() > 1000; });
    EXPECT_GT(window->width(), 1000.0F);
    // Two pages side by side (the app's): the canvas fits a design twice as wide, at least 1920 across.
    window->setDesignColumns(2);
    pumpUntil(*window, [&window](const WindowInput&) { return window->width() >= 1920.0F; });
    EXPECT_GE(window->width(), 1920.0F);
    window->setDesignColumns(1);
    pumpUntil(*window, [&window](const WindowInput&) { return window->width() < 1920.0F; });
    // An empty one (0 x 0) is drawn as 1 x 1.
    desktop.typed(id, ConfigureNotify, [](XEvent& e) {
        e.xconfigure.width = 0;
        e.xconfigure.height = 0;
    });
    pumpUntil(*window, [&window](const WindowInput&) { return window->width() == 960.0F; });

    // Obscured, then unmapped: not visible; mapped again and exposed.
    desktop.typed(id, VisibilityNotify, [](XEvent& e) { e.xvisibility.state = VisibilityFullyObscured; });
    pumpUntil(*window, [&window](const WindowInput&) { return !window->visible(); });
    EXPECT_FALSE(window->visible());
    desktop.typed(id, VisibilityNotify, [](XEvent& e) { e.xvisibility.state = VisibilityUnobscured; });
    desktop.typed(id, UnmapNotify, [](XEvent&) {});
    pumpUntil(*window, [](const WindowInput&) { return false; });
    EXPECT_FALSE(window->visible());
    desktop.typed(id, MapNotify, [](XEvent&) {});
    desktop.typed(id, Expose, [](XEvent& e) { e.xexpose.count = 1; });
    desktop.typed(id, Expose, [](XEvent& e) { e.xexpose.count = 0; });
    pumpUntil(*window, [&window](const WindowInput&) { return window->visible(); });
    EXPECT_TRUE(window->visible());

    // A message that is not the close button, then the close button.
    const Atom protocols = XInternAtom(desktop.display(), "WM_PROTOCOLS", False);
    const Atom deleteWindow = XInternAtom(desktop.display(), "WM_DELETE_WINDOW", False);
    desktop.typed(id, ClientMessage, [protocols](XEvent& e) {
        e.xclient.message_type = protocols;
        e.xclient.format = 32;
        e.xclient.data.l[0] = 12345;
    });
    input = pumpUntil(*window, [](const WindowInput&) { return false; });
    EXPECT_FALSE(input.close);
    desktop.typed(id, ClientMessage, [protocols, deleteWindow](XEvent& e) {
        e.xclient.message_type = protocols;
        e.xclient.format = 32;
        e.xclient.data.l[0] = static_cast<long>(deleteWindow);
    });
    input = pumpUntil(*window, [](const WindowInput& in) { return in.close; });
    EXPECT_TRUE(input.close);
}

// A frame: filled and outlined rectangles (empty ones skipped), text (spaces skipped, other characters as '?'),
// from an origin; nothing drawn before begin(). A pixmap too big for the server is an X error, logged, not fatal.
TEST(UiWindowX11, DrawsAFrame) {
    Xvfb server("800x600x24");   // smaller than the window asked for: made smaller, same shape
    if (!server.up()) GTEST_SKIP() << "no Xvfb";
    std::string error;
    auto window = openWindow("WheelBri draw", 960, 748, 1248, 972, error);
    ASSERT_NE(window, nullptr) << error;
    window->fill(0, 0, 10, 10, {1, 2, 3});   // before begin(): no pixmap yet, nothing drawn
    window->text(0, 0, "x", {1, 2, 3});
    window->begin({22, 24, 28});
    window->setOrigin(4, 8);
    window->fill(0, 0, 0, 10, {255, 0, 0});    // empty: skipped
    window->fill(0, 0, 10, -1, {255, 0, 0});
    window->fill(10, 10, 0.1F, 0.1F, {255, 0, 0});   // thinner than a pixel: still one
    window->outline(20, 20, 100, 50, {0, 255, 0});
    window->text(30, 30, "", {255, 255, 255});
    window->text(30, 30, "Hi there \x01\x7f\xc3", {255, 255, 255});
    window->setOrigin(0, 0);
    window->present();
    Desktop desktop(server.display());
    const Window id = desktop.find("WheelBri draw");
    ASSERT_NE(id, 0U);
    // A server-side error (a pixmap far too big): logged by the error handler, the window carries on.
    desktop.typed(id, ConfigureNotify, [](XEvent& e) {
        e.xconfigure.width = 40000;
        e.xconfigure.height = 40000;
    });
    pumpUntil(*window, [&window](const WindowInput&) { return window->width() > 2000; });
    window->begin({0, 0, 0});
    window->present();
    window->pump();
    desktop.typed(id, ConfigureNotify, [](XEvent& e) {
        e.xconfigure.width = 1248;
        e.xconfigure.height = 972;
    });
    pumpUntil(*window, [&window](const WindowInput&) { return window->width() < 2000; });
    window->begin({0, 0, 0});
    window->present();
}

// Folders: xdg-open (here a stand-in on PATH) opens one; without it, the error says so. pickFolder needs kdialog or
// zenity at their full paths: without them, it says what to do instead.
TEST(UiWindowX11, FoldersAndTheFolderWindow) {
    Xvfb server;
    if (!server.up()) GTEST_SKIP() << "no Xvfb";
    std::string error;
    auto window = openWindow("WheelBri folders", 960, 748, 1248, 972, error);
    ASSERT_NE(window, nullptr) << error;
    const std::filesystem::path bin = std::filesystem::temp_directory_path() / "wheelbri-x11-bin";
    std::filesystem::create_directories(bin);
    const std::string oldPath = std::getenv("PATH") != nullptr ? std::getenv("PATH") : "";
    setenv("PATH", bin.c_str(), 1);   // no xdg-open
    EXPECT_FALSE(window->openFolder("/tmp", error));
    EXPECT_NE(error.find("xdg-open"), std::string::npos) << error;
    {
        std::ofstream script(bin / "xdg-open");
        script << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::permissions(bin / "xdg-open", std::filesystem::perms::owner_all);
    EXPECT_TRUE(window->openFolder("/tmp", error)) << error;
    setenv("PATH", oldPath.c_str(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // it runs, its waiter collects it
    std::filesystem::remove_all(bin);

    if (access("/usr/bin/kdialog", X_OK) != 0 && access("/usr/bin/zenity", X_OK) != 0) {
        std::filesystem::path folder;
        EXPECT_FALSE(window->pickFolder("Game folder", folder, error));
        EXPECT_NE(error.find("no folder window here"), std::string::npos) << error;
        const char* const home = std::getenv("HOME");
        const std::string saved = home != nullptr ? home : "";
        unsetenv("HOME");
        EXPECT_FALSE(window->pickFolder("Game folder", folder, error));
        if (home != nullptr) setenv("HOME", saved.c_str(), 1);
    }
}

// The X server goes away: the window says it was closed, draws nothing, and is destroyed without touching the
// connection. A window opened afterwards on a display that works is not affected: it is shown and gets its input
// (the lost connection was once remembered for the whole process, and every later window stayed closed).
TEST(UiWindowX11, ZzDisplayGoesAway) {
#ifndef WE_HAVE_XIO_EXIT_HANDLER
    GTEST_SKIP() << "libX11 before 1.7 ends the process when the display goes away";
#endif
    Xvfb server;
    if (!server.up()) GTEST_SKIP() << "no Xvfb";
    std::string error;
    auto window = openWindow("WheelBri lost", 960, 748, 1248, 972, error);
    ASSERT_NE(window, nullptr) << error;
    window->begin({0, 0, 0});
    window->present();
    server.stop();
    WindowInput input;
    for (int i = 0; i < 100 && !input.close; ++i) {
        input = window->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(input.close);
    EXPECT_TRUE(window->pump().close);
    window->begin({0, 0, 0});
    window->fill(0, 0, 10, 10, {1, 1, 1});
    window->text(0, 0, "x", {1, 1, 1});
    window->present();
    window.reset();

    Xvfb next;
    ASSERT_TRUE(next.up());
    auto again = openWindow("WheelBri again", 960, 748, 1248, 972, error);
    ASSERT_NE(again, nullptr) << error;
    Desktop desktop(next.display());
    const Window id = desktop.find("WheelBri again");
    ASSERT_NE(id, 0U);
    pumpUntil(*again, [&again](const WindowInput&) { return again->visible(); });
    EXPECT_TRUE(again->visible());
    desktop.key(id, XK_m);
    input = pumpUntil(*again, [](const WindowInput& in) { return !in.keys.empty() || in.close; });
    EXPECT_EQ(input.keys, "m");
    EXPECT_FALSE(input.close);
}

}  // namespace
}  // namespace wheelbri::service::ui
