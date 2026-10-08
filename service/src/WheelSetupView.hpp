#pragma once

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

// The Wheel setup tab: defines the rig, i.e. which devices the user has (the wheel, pedals, a shifter, a button box)
// and what each of their controls is called and is for (Rig.hpp: a label and a role such as steering, throttle,
// d-pad up, left paddle, shift up). Every connected device is read at once; with Detect on, touching a control (pressing a
// button, moving an axis well away from where it rested) opens its popup: type the label, pick the role, Enter.
// A pedal's rest position also says whether it is inverted. Rigs are files in rigsFolder() (New, Rename, < >); the
// active one is remembered in the app's state file ([app] rig) and is what the Mapping tab maps from. It maps
// nothing itself. With a session running it reads the session's wheel only. Draws on the same 960 x 748 panel as
// MappingView, above its tab bar.
class WheelSetupView {
public:
    static constexpr int kWidth = 960;
    static constexpr int kHeight = 748;
    static constexpr float kContentHeight = kHeight - 30;   // the tab bar is below

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
    // WindowInput::text while a name is typed (printable characters, kBackspace, kEnter).
    void onText(const std::string& text);
    // WindowInput::keys: Escape stops typing, or closes the popup.
    void onKey(char key);
    // True while a name is typed: the window must not treat the keys as shortcuts then.
    [[nodiscard]] bool editingText() const { return editing_ != nullptr; }
    [[nodiscard]] bool dirty() const { return edit_ != saved_ || !rigSaved_; }

private:
    enum class Popup { None, Control, NewRig, RenameRig };

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
    [[nodiscard]] std::vector<DeviceId> shownList() const;
    [[nodiscard]] std::string systemName(const DeviceId& id) const;
    [[nodiscard]] int axisCount() const;
    [[nodiscard]] int buttonCount() const;
    [[nodiscard]] const RawInputState* liveRaw() const;

    // The rig.
    void loadRigs();
    void activate(const std::string& name);
    void save();
    void revert();
    void newRig();
    void renameRig();
    [[nodiscard]] RigDevice* shownDevice();               // in the edited rig, or null
    [[nodiscard]] const RigDevice* shownDevice() const;
    RigDevice* addShownDevice();                          // added to the rig if it is not in it (null: rig full)
    void removeShownDevice();

    // The control popup.
    void openControl(bool axis, int index, std::optional<float> rest);
    void closeControl(bool keep);

    void drawHeader();
    void drawAxes(float top);
    void drawButtons(float top);
    void drawControlPopup();
    void drawNamePopup();
    void drawNameField(float x, float y, std::string& name, std::size_t maxChars);

    // Clickable areas, rebuilt every draw; later ones win (the popup's come last).
    void hit(float x, float y, float w, float h, std::function<void()> action);
    float button(float x, float y, const std::string& label, std::function<void()> action, bool on = false,
                 bool enabled = true, float width = 0);

    [[nodiscard]] float axisValue(int axis) const;
    [[nodiscard]] bool buttonDown(int source) const;

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
    DeviceId shown_;
    bool detect_ = true;

    // The rig: as saved, and as edited.
    std::vector<std::string> rigNames_;
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
    std::string popupName_;
    std::string* editing_ = nullptr;
    std::size_t editingMax_ = DeviceLabels::kMaxLabel;

    int lastPressed_ = -1;
};

}  // namespace wheelbri::service
