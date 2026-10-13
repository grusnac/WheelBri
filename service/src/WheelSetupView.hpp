#pragma once

#include <algorithm>

#include "UiWindow.hpp"
#include "Wheel.hpp"

#include "wheelbri/RelistTimer.hpp"
#include "wheelbri/Rig.hpp"
#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The Rig tab: defines the rig, i.e. which devices the user has (the wheel, pedals, a shifter, a button box) and what
// each of their controls is called (Rig.hpp: a name only; WheelBri does not guess what a control is for, every wheel
// being different). There is no device to pick: every device of the rig (an unplugged one as saved) and every other
// connected game controller (what Windows' Game Controllers lists) is shown at once, one section each with its axes and
// its buttons. When they do not all fit they scroll: the mouse wheel, the scroll bar at the right (dragged or clicked),
// PageUp / PageDown, Up / Down, Home / End. The bottom-left corner lists them (at most 6 lines; the mouse wheel over it
// scrolls it; a line's click brings its section into view). With Detect on, touching a control on any device (pressing
// a button, moving an axis well away from where it rested) opens its popup: type the name, or press one of the names it
// offers ("It is": Steering, Throttle, A, Gear 1 ...: it only fills in the name), Enter. A pedal's rest position also
// says whether it is inverted. A device joins the rig when one of its controls is named (the rig's devices are the ones
// hidden from games, at most Rig::kMaxDevices); Forget takes one out. There are no dead zones: those belong in the
// device's own software. There is one rig, a file in rigsFolder() (remembered in the app's state file, [app] rig; "My
// rig" when it is first saved); it is what the Mapping tab maps from. It maps nothing itself. With a session running it
// reads the session's wheel only. Draws on the same panel as MappingView, above its tab bar: kWidth x kHeight in the
// design, bigger with the window (UiWindow.hpp): the axis bars wider, more button tiles per row, more of the devices
// shown; Save, Revert and Detect kept at the right, popups centred.
class WheelSetupView {
public:
    static constexpr int kWidth = 960;
    static constexpr int kHeight = 748;

    struct Settings {
        std::filesystem::path rigsFolder;      // <name>.ini per rig
        std::filesystem::path devicesFolder;   // the older per-device names: the first rig starts from them
        std::filesystem::path stateFile;       // [app] rig = the active rig
    };

    // A running session's wheel, as the window gets it.
    struct SessionWheel {
        WheelInfo info;
        int axes = 0;
        int buttons = 0;
        RawInputState raw;
    };

    WheelSetupView(ui::UiWindow& window, Settings settings);
    ~WheelSetupView();

    WheelSetupView(const WheelSetupView&) = delete;
    WheelSetupView& operator=(const WheelSetupView&) = delete;

    // Whether any rig has been saved (else the app opens on this tab).
    [[nodiscard]] static bool anyRig(const std::filesystem::path& rigsFolder);

    // Every frame while the tab shows: `session` is the running session's wheel, or null (then the tab reads every
    // connected device itself).
    void update(const SessionWheel* session);
    // Draws the tab (between the window's begin() and present(), origin at the panel's top left).
    void draw();
    // Closes the devices the tab reads itself (before a session opens them).
    void releaseWheel();

    void onClick(float x, float y);
    // Mouse wheel notches (> 0 = up): scrolls the devices, or the device list when the mouse is over it.
    void onScroll(int notches);
    // Every frame: where the mouse is (panel pixels) and whether the left button is held, to drag the devices' scroll
    // bar (a press on it comes through onClick first).
    void onPointer(float x, float y, bool leftDown);
    // WindowInput::text while a name is typed (printable characters, kBackspace, kEnter).
    void onText(const std::string& text);
    // WindowInput::keys: Escape stops typing, or closes the popup; PageUp / PageDown, Up / Down, Home / End scroll the
    // devices (not while a popup is open or a name is typed).
    void onKey(char key);
    // True while a name is typed: the window must not treat the keys as shortcuts then.
    [[nodiscard]] bool editingText() const { return editing_ != nullptr; }
    [[nodiscard]] bool dirty() const { return edit_ != saved_ || !rigSaved_; }

private:
    // The canvas beyond the design's width (pixels, characters), and the height above the tab bar.
    [[nodiscard]] float extraW() const { return std::max(0.0F, window_.width() - static_cast<float>(kWidth)); }
    [[nodiscard]] std::size_t extraChars() const { return static_cast<std::size_t>(extraW() / 8.0F); }
    [[nodiscard]] float contentHeight() const { return window_.height() - 30; }   // the tab bar is below
    enum class Popup { None, Control };

    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    // A connected device the tab reads itself (while stopped).
    struct Open {
        WheelInfo info;
        std::unique_ptr<Wheel> wheel;
        RawInputState raw;
        RawInputState rest;                 // where it was when Detect was armed (or it was opened)
        std::array<bool, RawInputState::kMaxAxes> armed{};   // an axis triggers Detect once, then waits to go back
        std::uint64_t previousButtons = 0;
        bool haveRaw = false;
        bool restTaken = false;
        int settleFrames = 0;               // frames read before the rest is taken (a device's first reads can be 0)
    };

    // Devices.
    bool refreshDevices(bool force);
    void openConnected();
    void detect(Open& device);
    [[nodiscard]] Open* openFor(const DeviceId& id);
    [[nodiscard]] const Open* openFor(const DeviceId& id) const;
    // The devices shown, in this order: the rig's (rig order; unplugged ones too, as saved), then the other
    // connected ones.
    [[nodiscard]] std::vector<DeviceId> shownDevices() const;
    // The bottom-left list: those, and any connected device not opened yet.
    [[nodiscard]] std::vector<DeviceId> listedDevices() const;
    // The button tiles a section shows: what the device has, or (not read now) up to its last named button.
    [[nodiscard]] int tileCount(const DeviceId& id) const;
    [[nodiscard]] bool connected(const DeviceId& id) const;
    // "Online" (connected) or "Offline"; "-" for a device the running session does not read (not known then).
    [[nodiscard]] std::string statusText(const DeviceId& id) const;
    [[nodiscard]] std::string systemName(const DeviceId& id) const;
    // What the device reports (0 = none, -1 = not known: then all are shown).
    [[nodiscard]] int axisCount(const DeviceId& id) const;
    [[nodiscard]] int buttonCount(const DeviceId& id) const;
    [[nodiscard]] const RawInputState* liveRaw(const DeviceId& id) const;

    // The rig.
    void loadRigs();
    void activate(const std::string& name);
    void save();
    void revert();
    // The device the control popup is for (focus_), in the edited rig, or null.
    [[nodiscard]] RigDevice* focusDevice();
    [[nodiscard]] const RigDevice* focusDevice() const;
    RigDevice* addFocusDevice();                          // added to the rig if it is not in it (null: rig full)
    void forget(const DeviceId& id);                      // out of the rig, with its names (Revert brings it back)

    // The control popup, for a control of `id`.
    void openControl(const DeviceId& id, bool axis, int index, std::optional<float> rest);
    void closeControl(bool keep);

    void drawHeader();
    // The devices' sections between `top` and `bottom` (scrolled by scroll_); the list and the hints below them.
    void drawDevices(float top, float bottom);
    [[nodiscard]] float sectionHeight(const DeviceId& id) const;
    void drawSection(const DeviceId& id, float y, float top, float bottom);
    void drawDeviceList(float top);
    void drawHints(float x, float top);
    void drawControlPopup();
    void drawPopupAxisBar(float x, float y, float width);
    [[nodiscard]] float bottomPanelHeight() const;
    [[nodiscard]] int tileColumns() const;
    void drawNameField(float x, float y, std::string& name, std::size_t maxChars);

    // Clickable areas, rebuilt every draw; later ones win (the popup's come last).
    void hit(float x, float y, float w, float h, std::function<void()> action);
    float button(float x, float y, const std::string& label, std::function<void()> action, bool on = false,
                 bool enabled = true, float width = 0);

    [[nodiscard]] float axisValue(const DeviceId& id, int axis) const;
    [[nodiscard]] bool buttonDown(const DeviceId& id, int source) const;

    ui::UiWindow& window_;
    Settings settings_;
    std::vector<Hit> hits_;

    // The devices: the session's wheel, or every connected one the tab opened itself.
    std::unique_ptr<WheelBackend> backend_;
    std::vector<WheelInfo> connected_;
    std::vector<Open> open_;
    // When to list the game controllers again: when the system says one came or went (platform::DeviceChanges).
    [[nodiscard]] static RelistTimer::Settings relistSettings();
    RelistTimer relist_{relistSettings()};
    bool listLogged_ = false;
    bool fromSession_ = false;
    WheelInfo sessionInfo_;
    int sessionAxes_ = 0;
    int sessionButtons_ = 0;
    RawInputState sessionRaw_;
    std::uint64_t sessionPreviousButtons_ = 0;
    DeviceId focus_;           // the device whose control the popup shows (the one last touched or clicked)
    bool detect_ = true;
    float scroll_ = 0;         // the devices' area, scrolled down this many pixels
    float scrollMost_ = 0;     // ...at most (from the last draw)
    float scrollRoom_ = 1;     // ...the area's height (from the last draw)
    void scrollBy(float pixels);
    // The devices' scroll bar as last drawn (shown when they do not all fit): dragged, or clicked to jump.
    struct Bar {
        float x = 0, y = 0, w = 0, h = 0;      // the track
        float thumbStart = 0, thumbSize = 0;   // along it
        bool shown = false;
    };
    Bar bar_;
    bool dragging_ = false;
    float dragGrab_ = 0;       // where on the thumb it was taken
    void dragTo(float y);
    float pointerX_ = 0;
    float pointerY_ = 0;
    // The bottom-left list: at most kListRows lines shown, scrolled by listScroll_ lines when there are more.
    std::size_t listScroll_ = 0;
    float listTop_ = 0, listBottom_ = 0;   // where it was drawn
    std::optional<DeviceId> reveal_;   // scroll to this device's section at the next draw (Detect found it)

    // The rig: as saved, and as edited.
    std::string rigName_;
    bool rigSaved_ = false;
    Rig saved_;
    Rig edit_;
    std::string status_;
    bool statusError_ = false;

    Popup popup_ = Popup::None;
    bool popupAxis_ = false;
    int popupIndex_ = -1;
    RigControl popupControl_;
    std::optional<float> popupRest_;
    std::string* editing_ = nullptr;
    std::size_t editingMax_ = DeviceLabels::kMaxLabel;

    int lastPressed_ = -1;      // the button last pressed, on lastPressedDevice_
    DeviceId lastPressedDevice_;
};

}  // namespace wheelbri::service
