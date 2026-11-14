// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

#include "HitList.hpp"
#include "DeviceWatch.hpp"
#include "HoverText.hpp"
#include "UiWindow.hpp"
#include "Wheel.hpp"

#include "wheelbri/ControlFold.hpp"
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
#include <utility>
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
// offers ("It is": Steering, Throttle, A, Gear 1 ...: kept whatever the name becomes; it fills in the name only when
// there is none, Button never), Enter. The popup says where a pedal rested (Invert is the Mapping tab's). A device
// joins the rig when one of its controls is named (the rig's devices are the ones
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
        // The app's listing of the connected game controllers, made on a thread of its own: the tab takes each new
        // one from it. Null (the CLI's window): the tab lists them itself, on this window's thread.
        const DeviceWatch* devices = nullptr;
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

    // Listening: the tab reads the devices (update) while another tab shows, for the Mapping tab's secondary popup;
    // every control touched (a button pressed, an axis moved as far as Detect needs) is kept for takeTouches, named
    // or not, and Detect opens no popup meanwhile. Off: nothing is kept.
    struct Touch {
        DeviceId device;
        bool axis = false;
        int index = 0;
    };
    void setListening(bool on);
    [[nodiscard]] std::vector<Touch> takeTouches() { return std::exchange(touches_, {}); }

    void onClick(float x, float y);
    // Mouse wheel notches (> 0 = up): scrolls the devices, or the device list when the mouse is over it.
    void onScroll(int notches);
    // Every frame: where the mouse is (panel pixels) and whether the left button is held, to drag the devices' scroll
    // bar (a press on it comes through onClick first).
    void onPointer(float x, float y, bool leftDown);
    // Every frame: where the mouse is for the hover text (`inside` false: it left the window), and the hover text
    // itself, drawn last (over the tab bar too).
    void setHover(float x, float y, bool inside) { hover_.setHover(x, y, inside); }
    void drawHoverText() const;
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
    // The tab bar is below.
    [[nodiscard]] float contentHeight() const { return window_.height() - 30; }
    enum class Popup { None, Control };

    // A connected device the tab reads itself (while stopped).
    struct Open {
        WheelInfo info;
        std::unique_ptr<Wheel> wheel;
        RawInputState raw;
        RawInputState rest;                 // where it was when Detect was armed (or it was opened)
        std::array<bool, RawInputState::kMaxAxes> armed{};   // an axis triggers Detect once, then waits to go back
        std::array<bool, RawInputState::kMaxAxes> touchArmed{};   // the same for a touch while listening
        SourceButtons previousButtons;
        bool haveRaw = false;
        bool restTaken = false;
        int settleFrames = 0;               // frames read before the rest is taken (a device's first reads can be 0)
    };

    // Devices.
    bool refreshDevices(bool force);
    void adoptListing(std::vector<WheelInfo> found);   // the devices connected now: logged when that changes
    void openConnected();
    void detect(Open& device);
    [[nodiscard]] const Open* openFor(const DeviceId& id) const;
    // The devices shown, in this order: the rig's (rig order; unplugged ones too, as saved), then the other
    // connected ones.
    [[nodiscard]] std::vector<DeviceId> shownDevices() const;
    // The bottom-left list: those, and any connected device not opened yet.
    [[nodiscard]] std::vector<DeviceId> listedDevices() const;
    // The button tiles a section shows: what the device has, or (not read now) up to its last named button.
    [[nodiscard]] int tileCount(const DeviceId& id) const;
    // What a section lists: once a device has a named control, only its named axes and buttons, the others folded
    // into one line each ("4 axes not named (1, 3, 4, 7)  [Show all]"); every one with Show all (showAll_, for the
    // tab, until Named only) or while the device has no name yet.
    struct Listed {
        ControlFold axes;
        ControlFold buttons;
        bool folds = false;   // a named control, or buttons past 63 (Show all / Named only applies)
    };
    [[nodiscard]] Listed listed(const DeviceId& id) const;
    // That line at `y` (kFoldRow tall): `text`, and Show all (`showAll`) or Named only.
    void drawFoldLine(float y, const std::string& text, bool showAll);
    [[nodiscard]] bool connected(const DeviceId& id) const;
    // "Connected" or "Not connected"; "-" for a device the running session does not read (not known then).
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
    // Save as you change: once a change is done (no name being typed, no popup open) the rig is written, and the rig
    // before it goes on the undo list (at most kMaxUndo). Not saved: the status says why, not tried again until the
    // next change.
    void autoSave();
    // Undo: drops a change that could not be saved, else goes back to the rig before the last change (saved again).
    void undo();
    // The device the control popup is for (focus_), in the edited rig, or null.
    [[nodiscard]] RigDevice* focusDevice();
    RigDevice* addFocusDevice();                          // added to the rig if it is not in it (null: rig full)
    // The control (not the popup's own) that is `kind` now, when only one may be (isAutoKind): "Pedals axis 1 Gas";
    // empty when none is.
    [[nodiscard]] std::string kindTakenBy(const std::string& kind) const;
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
    HitList hits_;

    // The devices: the session's wheel, or every connected one the tab opened itself.
    std::unique_ptr<WheelBackend> backend_;
    std::vector<WheelInfo> connected_;
    std::vector<Open> open_;
    // When to list the game controllers again: when the system says one came or went (platform::DeviceChanges).
    [[nodiscard]] static RelistTimer::Settings relistSettings();
    RelistTimer relist_{relistSettings()};
    std::uint64_t listedGeneration_ = 0;   // the DeviceWatch listing taken last (Settings::devices)
    bool listLogged_ = false;
    bool fromSession_ = false;
    WheelInfo sessionInfo_;
    int sessionAxes_ = 0;
    int sessionButtons_ = 0;
    RawInputState sessionRaw_;
    SourceButtons sessionPreviousButtons_;
    DeviceId focus_;           // the device whose control the popup shows (the one last touched or clicked)
    bool detect_ = true;
    bool listening_ = false;
    std::vector<Touch> touches_;   // what was touched while listening, not taken yet
    HoverText hover_;          // the explanations, under the mouse (Detect, Save, Show all...)
    bool showAll_ = false;     // the unnamed controls too (Show all), for every device
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
    static constexpr std::size_t kMaxUndo = 50;
    std::vector<Rig> undo_;            // the saved rigs before each change, oldest first
    bool undoing_ = false;             // the next save is an Undo's: nothing goes on the list
    std::optional<Rig> notSaved_;      // the change that could not be saved (not tried again)
    std::string status_;
    bool statusError_ = false;
    std::string afterSave_;            // the status once the change is saved (Forget's), instead of the usual one

    Popup popup_ = Popup::None;
    bool popupAxis_ = false;
    int popupIndex_ = -1;
    RigControl popupControl_;
    std::size_t popupHover_ = 0;   // the popup's hover areas start here (the tab's come before them)
    std::optional<float> popupRest_;
    std::string* editing_ = nullptr;
    std::size_t editingMax_ = DeviceLabels::kMaxLabel;

    int lastPressed_ = -1;      // the button last pressed, on lastPressedDevice_
    DeviceId lastPressedDevice_;
};

}  // namespace wheelbri::service
