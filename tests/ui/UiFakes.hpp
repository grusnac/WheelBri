#pragma once

// Fakes for the window's tests (wheelbri_ui_tests): a scripted, recording window; game controllers the test plugs in,
// moves and unplugs; and the link-time stand-ins that put them where the app looks for the real ones
// (ui::openWindow, startWheelBackend, DeviceWatch::systemLister: UiFakes.cpp). The real ones (UiWindowX11.cpp,
// WheelEvdev.cpp / WheelDinput.cpp, DeviceWatchSystem.cpp) are not linked into this binary.

#include "UiWindow.hpp"
#include "Wheel.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service::uitest {

// A window that keeps every draw call of the last finished frame (begin() .. present()) and answers pump() from a
// script (or, without one, with nothing).
class ScriptedWindow : public ui::UiWindow {
public:
    struct Text {
        float x, y;
        std::string value;
        ui::Colour colour;
    };
    struct Rect {
        float x, y, w, h;
        ui::Colour colour;
        bool filled;
    };
    struct Frame {
        ui::Colour background{};
        std::vector<Text> texts;
        std::vector<Rect> rects;
    };

    // One step of a script: `input` for one pump; `until`, when set, repeats the step (each pump gets `input` again)
    // until it is true (checked before the pump answers, so it sees the frame drawn after the previous pump), at most
    // `maxPumps` times (then the test fails: `timedOut`).
    struct Step {
        std::function<bool(ScriptedWindow&)> until;
        std::function<ui::WindowInput(ScriptedWindow&)> input;
        int maxPumps = 600;
        std::string what;
    };

    ScriptedWindow(float width = 960, float height = 748) { setSize(width, height); }

    void setSize(const float width, const float height) {
        canvasWidth_ = width;
        canvasHeight_ = height;
    }

    // The script; once it is used up, pump() says the window was closed (closeAtEnd) or nothing.
    std::deque<Step> script;
    bool closeAtEnd = true;
    bool timedOut = false;
    std::string timedOutAt;
    int pumps = 0;
    bool shown = true;
    bool notifies = false;            // notify()'s answer
    std::vector<std::string> notified;   // titles
    std::vector<std::filesystem::path> opened;   // openFolder()
    bool openFolderWorks = true;
    std::optional<std::filesystem::path> pickAnswer;   // pickFolder(): nullopt = cancelled
    std::string pickError;

    ui::WindowInput pump() override;
    [[nodiscard]] bool visible() const override { return shown; }
    void begin(ui::Colour background) override {
        current_ = Frame{};
        current_.background = background;
    }
    void fill(const float x, const float y, const float w, const float h, const ui::Colour colour) override {
        current_.rects.push_back({x + originX_, y + originY_, w, h, colour, true});
    }
    void outline(const float x, const float y, const float w, const float h, const ui::Colour colour) override {
        current_.rects.push_back({x + originX_, y + originY_, w, h, colour, false});
    }
    void text(const float x, const float y, const std::string_view value, const ui::Colour colour) override {
        current_.texts.push_back({x + originX_, y + originY_, std::string(value), colour});
    }
    void present() override {
        last = current_;
        ++frames;
    }
    bool openFolder(const std::filesystem::path& folder, std::string& error) override {
        opened.push_back(folder);
        if (!openFolderWorks) error = "no file manager";
        return openFolderWorks;
    }
    bool pickFolder(const std::string&, std::filesystem::path& folder, std::string& error) override {
        if (!pickAnswer) {
            error = pickError;
            return false;
        }
        folder = *pickAnswer;
        return true;
    }
    bool notify(const std::string& title, const std::string&) override {
        notified.push_back(title);
        return notifies;
    }
    [[nodiscard]] std::string describe() const override { return "scripted"; }

    // Windows' logoff / shutdown messages, as UiWindowWin32 delivers them.
    void endSession() {
        if (endSession_) endSession_();
    }
    void queryEndSession() {
        if (queryEndSession_) queryEndSession_();
    }
    [[nodiscard]] bool hasEndSession() const { return static_cast<bool>(endSession_); }

    // The frame drawn so far (before present()), e.g. for views drawn without begin()/present().
    [[nodiscard]] const Frame& drawing() const { return current_; }
    void clearDrawing() { current_ = Frame{}; }
    // Makes the frame drawn so far the last one (views drawn directly, not through an app loop).
    void finish() { present(); }

    Frame last;
    int frames = 0;

    // The first text of the last frame equal to `value` (or containing it: `part`), else null.
    [[nodiscard]] const Text* find(const std::string& value, bool part = false) const;
    [[nodiscard]] bool has(const std::string& value, bool part = false) const { return find(value, part) != nullptr; }
    // Every text of the last frame, one per line (for failure messages).
    [[nodiscard]] std::string dump() const;
    // A click on the middle of the first character of `value` (in the last frame); false when it is not drawn.
    [[nodiscard]] std::optional<ui::Click> clickOn(const std::string& value, bool part = false) const;

private:
    Frame current_;
};

// Input helpers.
[[nodiscard]] ui::WindowInput click(float x, float y);
[[nodiscard]] ui::WindowInput keys(std::string keys);
[[nodiscard]] ui::WindowInput typed(std::string text);
[[nodiscard]] ui::WindowInput nothing();

// --- game controllers ---------------------------------------------------------------------------------------------

struct FakeDevice {
    WheelInfo info;
    int axes = 8;
    int buttons = 16;
    RawInputState raw;
    bool plugged = true;
    bool openFails = false;
    bool pollFails = false;   // the next polls say it is gone
};

// What the fake system has connected. Thread safe (the app's DeviceWatch lists on its own thread).
class FakeDevices {
public:
    static FakeDevices& shared();

    std::shared_ptr<FakeDevice> add(const std::string& name, std::uint16_t vendorId, std::uint16_t productId,
                                    int axes = 8, int buttons = 16);
    void clear();
    // Changes a device under the lock (its raw state, plugged...).
    void change(const std::shared_ptr<FakeDevice>& device, const std::function<void(FakeDevice&)>& edit);
    // Tells the app a device came or went, as the system's notification would.
    static void announce();

    [[nodiscard]] std::vector<WheelInfo> list() const;
    [[nodiscard]] std::shared_ptr<FakeDevice> find(const std::string& path) const;
    [[nodiscard]] bool poll(const std::shared_ptr<FakeDevice>& device, RawInputState& raw) const;

    std::atomic<bool> backendFails{false};    // startWheelBackend() gives null
    std::atomic<bool> listerUnavailable{false};   // DeviceWatch::systemLister() gives nullopt
    std::atomic<bool> listerHolds{false};         // ...and takes its time (until cleared, at most 5 s)
    std::atomic<int> backendStarts{0};

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<FakeDevice>> devices_;
    int next_ = 0;
};

// --- the window ui::openWindow gives ------------------------------------------------------------------------------

// The next window ui::openWindow() opens: made by this factory (null: it fails with `openError`).
struct WindowFactory {
    std::function<std::unique_ptr<ui::UiWindow>()> make;
    std::string openError = "no display";
    std::vector<std::string> titles;   // what was opened
    static WindowFactory& shared();
};

// A window owned by the factory's caller but scripted by the test: the factory hands out this proxy, which forwards
// everything to `target` (owned by the test, so it can be inspected after the app closed it).
class ProxyWindow final : public ui::UiWindow {
public:
    explicit ProxyWindow(ScriptedWindow& target) : target_(target) {
        canvasWidth_ = target.width();
        canvasHeight_ = target.height();
    }
    ui::WindowInput pump() override;
    [[nodiscard]] bool visible() const override { return target_.visible(); }
    void begin(ui::Colour background) override {
        sync();
        target_.setOrigin(originX_, originY_);
        target_.begin(background);
    }
    void fill(float x, float y, float w, float h, ui::Colour c) override {
        target_.setOrigin(originX_, originY_);
        target_.fill(x, y, w, h, c);
    }
    void outline(float x, float y, float w, float h, ui::Colour c) override {
        target_.setOrigin(originX_, originY_);
        target_.outline(x, y, w, h, c);
    }
    void text(float x, float y, std::string_view v, ui::Colour c) override {
        target_.setOrigin(originX_, originY_);
        target_.text(x, y, v, c);
    }
    void present() override { target_.present(); }
    bool openFolder(const std::filesystem::path& f, std::string& e) override { return target_.openFolder(f, e); }
    bool pickFolder(const std::string& t, std::filesystem::path& f, std::string& e) override {
        return target_.pickFolder(t, f, e);
    }
    bool notify(const std::string& title, const std::string& text) override { return target_.notify(title, text); }
    [[nodiscard]] std::string describe() const override { return target_.describe(); }
    // The app's end-session handlers, for the test to call.
    // As UiWindowWin32 does on WM_ENDSESSION: the window is closed from then on, and the app's handler runs.
    void endSession() {
        if (endSession_) {
            closed_ = true;
            endSession_();
        }
    }
    void queryEndSession() {
        if (queryEndSession_) queryEndSession_();
    }

private:
    bool closed_ = false;
    void sync() {
        canvasWidth_ = target_.width();
        canvasHeight_ = target_.height();
    }
    ScriptedWindow& target_;
};

// The proxy the factory handed out last (to call its end-session handlers); null before.
ProxyWindow* lastProxy();
// A factory that hands out a proxy of `target`.
std::function<std::unique_ptr<ui::UiWindow>()> proxyOf(ScriptedWindow& target);

// --- the user's folders -----------------------------------------------------------------------------------------

// Points the user's folders (XDG_CONFIG_HOME, XDG_STATE_HOME; LOCALAPPDATA on Windows) at a fresh temporary folder
// for the test's lifetime, and puts them back after.
class UserFolders {
public:
    explicit UserFolders(const std::string& name);
    ~UserFolders();
    UserFolders(const UserFolders&) = delete;
    UserFolders& operator=(const UserFolders&) = delete;
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

}  // namespace wheelbri::service::uitest
