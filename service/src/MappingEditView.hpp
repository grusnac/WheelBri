#pragma once

#include "ConfigPicker.hpp"
#include "UiWindow.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The app's Mapping tab while stopped: at the top the target (ConfigPicker: G920 virtual device or G920 in-game shim),
// whose axes, buttons and D-pad are fixed, then what drives each of them, from the controls the Rig tab named (the
// wheel's and the accessories'). Without a saved mapping of the user's it is WheelBri's default (the target's built-in
// config), exactly what Start uses. Every source is picked by hand from the named controls (WheelBri does not guess
// what a control is for: every wheel is different); Save keeps the user's mapping (my-<built-in name>, comments and
// other settings kept), Revert drops unsaved changes, Default drops the saved mapping. A source the rig has not named
// shows "not in the rig". The live view (the session's Mapping tab) shows the result once started.
//
// It also draws the app's Game profiles tab (Page::Games), which edits the same mapping: the built-in game profiles
// (GameProfiles.hpp) in a list, and for the one picked what its profile changes, the user's own values for it (its
// steering rotation, force feedback gain and own settings: [game_rotation], [game_ffb_gain], [game_settings], saved
// with the same Save) and what its settings file holds now. While a session runs it is shown locked, with the game that
// runs (setGameNow); the app switches to it and picks the game when a profiled game starts (GameTabFollower).
class MappingEditView {
public:
    // The canvas this tab is drawn on: at least 960 x 748, bigger in a bigger window (UiWindow.hpp); the tab bar
    // takes its bottom 30. Fields, bars and pickers grow with it, buttons at the right stay at the right.

    enum class Page { Mapping, Games };
    void setPage(const Page page) {
        if (page != page_) {
            field_ = Field::None;   // typing and a picker end with the page they were on
            mode_ = Mode::Idle;
            dragging_ = false;
        }
        page_ = page;
    }
    [[nodiscard]] Page page() const { return page_; }
    // The game the Games page shows (a profile id); an unknown one is ignored.
    void selectGame(const std::string& id);

    // What the running session says about games, for the Games page (UiFrame's game fields).
    struct GameNow {
        bool session = false;           // a session runs: games are looked for every 2 s
        std::string id;                 // the profile in use now; empty: none runs
        std::uint32_t pid = 0;
        std::uint64_t forMs = 0;
        std::string rotation;           // the rotation it steers with, when not the mapping's
        float ffbGain = 1.0F;
        std::string warning;
        std::map<std::string, std::string> settings;   // by profile id: what happened to its own settings
    };
    void setGameNow(GameNow now) { now_ = std::move(now); }

    // `rigsFolder` / `stateFile`: the rig the Wheel setup tab made (its active one, [app] rig), whose controls are
    // the sources.
    MappingEditView(ui::UiWindow& window, std::filesystem::path rigsFolder, std::filesystem::path stateFile,
                    ConfigPicker& picker);

    // Every frame while the tab shows: reads the config again when it changed, and the rig now and then (the Rig tab
    // may just have saved it; the rig's mapping follows it).
    void update();
    void draw();
    // While a session runs or starts the way and the config cannot change.
    void setLocked(bool locked);

    void onClick(float x, float y);
    void onKey(char key);
    // Typing in a number field (a rotation): printable characters, ui::kBackspace, ui::kEnter.
    void onText(const std::string& text);
    // The pointer every frame (panel coordinates): drags the game full lock slider while the left button is held.
    void onPointer(float x, float y, bool leftDown);
    // Where the mouse is (panel coordinates) every frame, and whether it is over the tab: a hovered greyed control in
    // a picker shows where it is used.
    void setHover(const float x, const float y, const bool inside) {
        hoverX_ = x;
        hoverY_ = y;
        hoverInside_ = inside;
    }
    // A number field is being typed in: letters are not the window's shortcuts then, Escape stops typing.
    [[nodiscard]] bool editingText() const { return field_ != Field::None; }
    [[nodiscard]] bool dirty() const { return loaded_ && edit_ != saved_; }
    // A popup (pick a source, the wheel) covers the tab: the window's own controls on it wait.
    [[nodiscard]] bool popupOpen() const { return mode_ != Mode::Idle; }

private:
    enum class Mode { Idle, Pick, PickWheel, PickAlt };
    // What a pick is for: one of the four axes, a D-pad direction, a target button.
    enum class Slot { Axis, Hat, Button };
    // A number field: the steering rotation, a game's own rotation (`fieldIndex_` = its gameProfiles() index), or
    // a game's own setting (`fieldIndex_` = game index * kMaxGameSettings + the setting's index), or a game's force
    // feedback gain (`fieldIndex_` = its gameProfiles() index).
    enum class Field { None, Rotation, PerGame, GameSetting, FfbGain };

    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };
    struct Tip {
        float x, y, w, h;
        std::string text;
    };
    // How a picker shows a rig control (each control is used once in a mapping: usesOf, MappingEdit.hpp): `mine`
    // what is being picked has it; `taken` something else has it (greyed, `tip` says what); `twice` it is used more
    // than once (red: Save refuses).
    struct EntryState {
        bool mine = false;
        bool taken = false;
        bool twice = false;
        std::string tip;
    };

    [[nodiscard]] float canvasW() const { return window_.width(); }
    [[nodiscard]] float canvasH() const { return window_.height(); }
    [[nodiscard]] float contentH() const { return window_.height() - 30; }   // the tab bar is below
    // What the canvas has beyond the 960 the tab was designed for, in pixels and in characters.
    [[nodiscard]] float extraW() const { return std::max(0.0F, window_.width() - 960.0F); }
    [[nodiscard]] std::size_t extraChars() const { return static_cast<std::size_t>(extraW() / 8.0F); }
    // The popups (pickers): the canvas less a margin; their entries in as many columns of at least 174 as fit (5..8).
    [[nodiscard]] float popupW() const { return window_.width() - 48; }
    [[nodiscard]] float popupH() const { return window_.height() - 108; }
    [[nodiscard]] int popupColumns() const {
        return std::clamp(static_cast<int>((popupW() - 24 + 4) / (174 + 4)), 5, 8);
    }
    [[nodiscard]] float entryW(const int columns) const {
        return (popupW() - 24 - 4 * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    }
    // The game full lock slider: 400 wide in the design, half of the extra width more.
    [[nodiscard]] float sliderW() const { return 400 + extraW() / 2; }
    // The Games page: characters right of the list, from the values' column, and where a file value goes.
    [[nodiscard]] std::size_t gameChars() const { return 92 + extraChars(); }
    [[nodiscard]] std::size_t valueChars() const { return 71 + extraChars(); }
    [[nodiscard]] float fileColumnX() const { return window_.width() - 4 - 13 * 8; }

    void load();
    // A mapped source that the active rig has not named (the default was made for another rig).
    [[nodiscard]] bool missing(const SourceRef& source, bool axis) const;
    void save();
    void dropOwnEdits();
    void refreshDevices();
    void changeWheel(const DeviceLabels& wheel);
    [[nodiscard]] const RigControl* rigControl(const SourceRef& source, bool axis) const;

    [[nodiscard]] const DeviceLabels* wheelLabels() const;
    [[nodiscard]] const DeviceLabels* labelsOf(const SourceRef& source) const;
    // "Moza R3: A": the device and the control's name (its number when it has none).
    [[nodiscard]] std::string describe(const SourceRef& source, bool axis) const;
    // The hover text: name and number ("Moza R3: 'A', button 0").
    [[nodiscard]] std::string describeFull(const SourceRef& source, bool axis) const;
    [[nodiscard]] std::string deviceText(const SourceRef& source) const;
    [[nodiscard]] SourceRef& slot(Slot kind, std::size_t index);

    void drawSource(float x, float y, float w, Slot kind, std::size_t index);
    void drawWheelRow();   // the wheel (it plays force feedback), Change
    void drawPicker();
    void drawWheelPicker();
    // A target button's secondary sources (MappingModel::secondaries): its field (the first, "+N" more), and its
    // picker (the rig's buttons and its throttle, brake, clutch and handbrake axes, each toggled; an axis' threshold).
    void drawAltSource(float x, float y, float w, std::size_t index);
    void drawAltPicker();
    [[nodiscard]] std::string describeAlt(const ButtonRef& alt) const;
    [[nodiscard]] std::string axisName(const SourceRef& source) const;
    // "Horn", "Lever > 50%": its rig name, or "axis N".
    [[nodiscard]] std::string shortAlt(const ButtonRef& alt) const;
    [[nodiscard]] EntryState entryState(const ControlRef& control, const ControlUse& self) const;
    // A picker's entry for a rig control: highlighted when `state.mine`, greyed (no click, a tooltip) when taken.
    void drawEntry(float x, float y, float w, const std::string& text, const EntryState& state,
                   std::function<void()> action);
    void tip(float x, float y, float w, float h, std::string text);
    void drawTooltip();
    // The STEERING ROTATION section from `y`: the rotation, and that the wheel's own software must match it.
    void drawRotation(float y);
    // The Games page: the list of profiles, and the picked one's.
    void drawGames();
    // The picked game from `y` (x from kGameX): what its profile changes, the user's values, its settings file.
    float drawGameProfile(std::size_t index, float y);
    float drawGameValues(std::size_t index, float y);
    float drawGameSettings(std::size_t index, float y);
    [[nodiscard]] std::size_t selectedGameIndex() const;
    // A number field showing `value` (or what is typed in it); amber when it differs from the saved mapping.
    void drawField(float x, float y, float w, Field field, const std::string& value, bool changed,
                   std::size_t index = 0);
    [[nodiscard]] std::string fieldText(Field field, std::size_t index) const;
    void startTyping(Field field, std::size_t index = 0);
    // Applies what was typed: false (and the reason in the status) when it is not a usable value.
    bool commitTyping();
    void setRotationFromX(float x);
    float button(float x, float y, const std::string& label, std::function<void()> action, bool on = false,
                 bool enabled = true, float width = 0);
    void hit(float x, float y, float w, float h, std::function<void()> action);

    ui::UiWindow& window_;
    std::filesystem::path rigsFolder_;
    std::filesystem::path stateFile_;
    std::string rigName_;
    Rig rig_;
    ConfigPicker& picker_;
    std::vector<Hit> hits_;
    std::vector<Tip> tips_;
    std::size_t popupTips_ = 0;   // tips_ from here on are the open popup's
    float hoverX_ = 0.0F;
    float hoverY_ = 0.0F;
    bool hoverInside_ = false;
    std::size_t altFocus_ = 0;   // the secondary whose threshold the picker edits (when it is an axis)

    std::uint64_t seenChanges_ = ~0ULL;
    std::string path_;
    bool builtIn_ = false;
    std::string fileText_;
    bool loaded_ = false;
    std::unique_ptr<IDeviceProfile> profile_;
    MappingModel saved_;
    MappingModel edit_;
    std::vector<DeviceLabels> devices_;
    std::uint64_t devicesReadMs_ = 0;

    Mode mode_ = Mode::Idle;
    Slot pickKind_ = Slot::Axis;
    std::size_t pickIndex_ = 0;
    bool locked_ = false;
    std::string status_;
    bool statusError_ = false;

    std::string targetName_ = "g920";   // [emulator] target_profile: which games' advice fits
    Field field_ = Field::None;
    std::size_t fieldIndex_ = 0;
    std::string typed_;
    bool dragging_ = false;   // the slider, from a press on it until the left button is let go
    float clickX_ = 0.0F;     // where the click being answered is (the slider's press)

    Page page_ = Page::Mapping;
    std::string selectedGame_;   // the Games page's game (a profile id); empty: the first
    GameNow now_;
    // The picked game's settings file as last read (at most every 2 s, and when the game or the saved values change).
    std::optional<GameSettingsFileState> fileState_;
    std::string fileStateKey_;
    std::uint64_t fileStateReadMs_ = 0;
};

}  // namespace wheelbri::service
