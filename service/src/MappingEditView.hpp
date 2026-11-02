#pragma once

#include "ConfigPicker.hpp"
#include "ShimGamesView.hpp"
#include "UiWindow.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
// It also draws the app's Games tab (Page::Games), which edits the same mapping: the built-in game profiles
// (GameProfiles.hpp) in a list, and for the one picked what its profile changes, the user's own values for it (its
// steering rotation, pedals, buttons and own settings: [game_rotation], [game_tuning], [game_settings],
// saved in the same mapping as they change: that page has no Default, Save or Revert, which are the Mapping tab's) and what
// its settings file holds now. While a session runs it is shown locked, with the game that
// runs (setGameNow); the app switches to it and picks the game when a profiled game starts (GameTabFollower).
class MappingEditView {
public:
    // The canvas this tab is drawn on: at least 960 x 748, bigger in a bigger window (UiWindow.hpp); the tab bar
    // takes its bottom 30. Fields, bars and pickers grow with it, buttons at the right stay at the right.

    enum class Page { Mapping, Games };
    void setPage(const Page page) {
        if (page != page_) {
            field_ = Field::None;   // typing and a picker end with the page they were on
            if (mode_ == Mode::PickAlt) closeAltPicker();
            mode_ = Mode::Idle;
            dragging_ = false;
        }
        page_ = page;
    }
    [[nodiscard]] Page page() const { return page_; }
    // The game the Games page shows (a profile id); an unknown one is ignored.
    void selectGame(const std::string& id);
    // The game folders for Mode Shim (the App's): the Games page shows each game's folder and its shim, and the
    // folders of games WheelBri does not know.
    void setShimGames(ShimGamesView* shims) { shims_ = shims; }

    // What the running session says about games, for the Games page (UiFrame's game fields).
    struct GameNow {
        bool session = false;           // a session runs: games are looked for every 2 s
        std::string id;                 // the known game running now; empty: none runs
        bool profileApplies = false;    // its built-in profile is in use
        std::uint32_t pid = 0;
        std::uint64_t forMs = 0;
        std::string rotation;           // the rotation it steers with, when not the mapping's
        std::string warning;
        // The game's force feedback now, per kind of effect (GameTuning.hpp FfbGroup): the strongest it asks and what
        // reaches the wheel (after the safety limits), 0..1. `ffbLive`: the session has force feedback.
        bool ffbLive = false;
        std::array<float, kFfbGroupCount> asked{};
        std::array<float, kFfbGroupCount> sent{};
    };
    // Also learns each game's settings file: when a known game ends, the one it wrote while it ran (learnGameFile).
    void setGameNow(GameNow now);

    // `rigsFolder` / `stateFile`: the rig the Wheel setup tab made (its active one, [app] rig), whose controls are
    // the sources.
    MappingEditView(ui::UiWindow& window, std::filesystem::path rigsFolder, std::filesystem::path stateFile,
                    ConfigPicker& picker);

    // Every frame while the tab shows: reads the config again when it changed, and the rig now and then (the Rig tab
    // may just have saved it; the rig's mapping follows it).
    void update();
    void draw();
    // The hover text, after everything else in the window (the app draws the shim's game row over the tab).
    void drawTooltip();
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
    // A number field is being typed in, or a picker's find box (it takes what is typed while it is open):
    // letters are not the window's shortcuts then, Escape stops typing (clears the find box, then closes the popup).
    [[nodiscard]] bool editingText() const {
        return field_ != Field::None || mode_ == Mode::PickAlt || mode_ == Mode::Pick;
    }
    // The mouse wheel (notches, > 0 = up): scrolls a picker's list.
    void onScroll(int notches);
    // A picker wants the rig's controls as they are touched (the app reads the devices meanwhile, as the Rig tab does)
    // and onRigTouch gets each one. The secondary popup adds a named control to the button's secondaries (never takes
    // one off: only a click does that) and flashes its entry; the source picker picks it and closes, unless it is used
    // elsewhere (its entry flashes: a click moves it). Anything else says why not.
    [[nodiscard]] bool wantsRigTouches() const {
        return (mode_ == Mode::PickAlt || mode_ == Mode::Pick) && !locked_;
    }
    void onRigTouch(const DeviceId& device, bool axis, int index);
    [[nodiscard]] bool dirty() const { return loaded_ && edit_ != saved_; }
    // Can be edited now: anything while no session runs; the Games page's values also while one does (the
    // session reads them again when they are saved: live tuning).
    [[nodiscard]] bool editable() const { return !locked_ || page_ == Page::Games; }
    // A popup (pick a source, the wheel) covers the tab: the window's own controls on it wait.
    // A short message after something was done ("Mapping saved"): taken once, the app shows it for a few seconds.
    [[nodiscard]] std::string takeToast() { return std::exchange(toast_, std::string()); }
    [[nodiscard]] bool popupOpen() const { return mode_ != Mode::Idle; }

    // What the tab asks the system: the Open dialog (platform::pickFile, canPickFile), the Documents folder, the
    // running programs and the clock. Each one left empty is the system's own; the tests give their own.
    struct System {
        std::function<std::optional<std::filesystem::path>(const std::string& title, const std::filesystem::path& folder,
                                                           const std::string& fileName)>
            pickFile;
        std::function<bool()> canPickFile;
        std::function<std::filesystem::path()> documentsFolder;
        std::function<std::optional<std::vector<RunningProgram>>()> runningPrograms;
        std::function<std::uint64_t()> nowMs;
    };
    void setSystem(System system) { system_ = std::move(system); }

private:
    // AddButton: the Games page's Add button... popup (the target's buttons, then Pick for the one chosen).
    enum class Mode { Idle, Pick, PickAlt, AddButton };
    // What a pick is for: one of the four axes, a D-pad direction, a target button.
    // GameButton: a target button's source while the picked game runs (its own value, GameTuning.hpp).
    enum class Slot { Axis, Hat, Button, GameButton };
    // A number field: the steering rotation, a game's own rotation (`fieldIndex_` = its gameProfiles() index; PerGame
    // in its Mapping section, GameSteering in its Game config: two places, one value), or
    // a game's own setting (`fieldIndex_` = game index * kMaxGameSettings + the setting's index), or the threshold of an axis
    // that presses a button.
    enum class Field { None, Rotation, CentringSpring, PerGame, GameSteering, GameSetting, Threshold };
    // The Games page's sub-tabs for the picked game.
    enum class GameTab { Axes, Ffb, Buttons, File };

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
    // The tab bar is below, and the app's notice above it when there is one.
    [[nodiscard]] float contentH() const { return window_.height() - 30 - window_.bottomReserve(); }
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
    // The Games page: the list on the left (its width dragged by the bar right of it, kept in app.ini as
    // games_list_width), the picked game right of it from gameX(), its values' fields from gameValueX(). The list is
    // kGameListMinW wide at least; at most half the canvas, and never so wide that the game's side has less than
    // kGameSideMinW.
    static constexpr float kGameListMinW = 196;
    static constexpr float kGameSideMinW = 616;
    [[nodiscard]] float maxGameListW() const {
        return std::max(kGameListMinW, std::min(canvasW() / 2 - 24, canvasW() - 24 - kGameSideMinW));
    }
    [[nodiscard]] float gameListW() const { return std::clamp(gameListW_, kGameListMinW, maxGameListW()); }
    [[nodiscard]] float gameX() const { return gameListW() + 20; }
    [[nodiscard]] float gameValueX() const { return gameX() + 168; }
    // What a wider list takes from the game's side, in characters.
    [[nodiscard]] std::size_t listChars() const {
        return static_cast<std::size_t>((gameListW() - kGameListMinW) / 8.0F);
    }
    // The Games page: characters right of the list, from the values' column, and where a file value goes.
    [[nodiscard]] std::size_t gameChars() const { return 92 + extraChars() - listChars(); }
    [[nodiscard]] std::size_t valueChars() const { return 71 + extraChars() - listChars(); }
    [[nodiscard]] float fileColumnX() const { return window_.width() - 4 - 13 * 8; }

    void load();
    // A mapped source that the active rig has not named (the default was made for another rig).
    [[nodiscard]] bool missing(const SourceRef& source, bool axis) const;
    void save();
    // Save as you change (the Mapping page): once a change is done (no typing, dragging or picker open), it is
    // written, and the mapping before it goes on the undo list (at most kMaxUndo). Not saved (a control used twice,
    // a file that cannot be written): the status says why, and it is not tried again until the next change.
    void autoSave();
    // Undo: drops a change that could not be saved, else goes back to the mapping before the last one (saved again).
    void undo();
    // The Games page's values ([game_rotation], [game_settings], [game_tuning]) differ from the saved ones.
    [[nodiscard]] bool gameValuesChanged() const;
    void saveGameValues();   // saves those alone, as they change
    void dropOwnEdits();
    void refreshDevices();
    // The buttons whose game number changed when the user's Logitech driver mapping was copied for Native
    // ([app] renumbered_<target>, ConfigPicker): "pending" becomes the mapped ones, then a row's mark goes when it
    // is changed, all of them with "Got it".
    void readRenumbered();
    void writeRenumbered();
    void dropChangedRenumbered(const MappingModel& before, const MappingModel& after);
    [[nodiscard]] bool renumbered(std::size_t b) const;
    [[nodiscard]] const RigControl* rigControl(const SourceRef& source, bool axis) const;

    [[nodiscard]] const DeviceLabels* wheelLabels() const;
    [[nodiscard]] const DeviceLabels* labelsOf(const SourceRef& source) const;
    // "Moza R3: A": the device and the control's name (its number when it has none).
    // Target button `b` (as named) by the number the game sees it by with the chosen Button order: from 1 as
    // Logitech's driver counts, from 0 in the native order (gameButtonNumber). The INI's target_N stays `b`.
    [[nodiscard]] int gameNumber(std::size_t b) const;
    [[nodiscard]] std::string describe(const SourceRef& source, bool axis) const;
    // The hover text: name and number ("Moza R3: 'A', button 0").
    [[nodiscard]] std::string describeFull(const SourceRef& source, bool axis) const;
    [[nodiscard]] std::string deviceText(const SourceRef& source) const;
    [[nodiscard]] SourceRef& slot(Slot kind, std::size_t index);
    // Whether that input follows the Rig (from_rig, AutoMap.hpp): its source is what the Rig gives it, saved as no
    // line.
    [[nodiscard]] bool followsRig(Slot kind, std::size_t index) const;

    void drawSource(float x, float y, float w, Slot kind, std::size_t index);
    void drawPicker();
    // The Buttons sub-tab's Add button...: the target's buttons in its own groups; one adds its row to the sub-tab
    // (pendingButtons_), where its main and its secondaries are then changed.
    void drawAddButton();
    // A target button's secondary sources (MappingModel::secondaries, or a game's): "or ..." chips from `x`, as many as
    // fit `w` ("+N" for the rest), each doing `action` (none: not clickable); `twiceFor`: the Mapping tab's button
    // whose chips a control used twice outlines red. The row's "+" button that opens the picker too; and the picker
    // (the rig's buttons and its named axes, each toggled; an axis' threshold).
    void drawChips(float x, float y, float w, const std::vector<ButtonRef>& list, std::optional<std::size_t> twiceFor,
                   bool dim, const std::function<void()>& action);
    void drawPlus(float x, float y, std::size_t index);
    [[nodiscard]] float chipsWidth(const std::vector<ButtonRef>& list) const;
    [[nodiscard]] static float textWidth(const std::string& text);
    void drawAltPicker();
    void openAltPicker(std::size_t index);
    // The secondary picker for the picked game's button `index`: it edits the game's own list, a copy of the Mapping
    // tab's when the game had none (pickKind_ GameButton).
    void openGameAltPicker(std::size_t index);
    // Closes the secondary picker; a game's list left the same as the Mapping tab's goes (it follows the Mapping tab).
    void closeAltPicker();
    // What the secondary picker edits: the Mapping tab's list of pickIndex_, or the picked game's.
    std::vector<ButtonRef>& altList();
    [[nodiscard]] bool altForGame() const { return mode_ == Mode::PickAlt && pickKind_ == Slot::GameButton; }
    // Opens the source picker for `kind` `index` (find box, scroll and notes from the start).
    void openPicker(Slot kind, std::size_t index);
    // The source picker's choice of a control used elsewhere: taken from every other use, then picked.
    void moveHere(const SourceRef& source);
    // What the source picker picks for, as usesOf names it.
    [[nodiscard]] ControlUse pickUse() const;
    // What it picks for, said: "Throttle", "D-pad Up", "button 1 A" (" in NFS 2015" for one game).
    [[nodiscard]] std::string pickWhat() const;
    [[nodiscard]] std::string describeAlt(const ButtonRef& alt) const;
    [[nodiscard]] std::string axisName(const SourceRef& source) const;
    // "Horn", "Lever > 50%": its rig name, or "axis N".
    [[nodiscard]] std::string shortAlt(const ButtonRef& alt) const;
    [[nodiscard]] EntryState entryState(const ControlRef& control, const ControlUse& self) const;
    // A picker's entry for a rig control: highlighted when `state.mine`, greyed (a tooltip) when taken: no click,
    // unless `takenClicks` (the source picker: a click moves it here).
    void drawEntry(float x, float y, float w, const std::string& text, const EntryState& state,
                   std::function<void()> action, bool takenClicks = false);
    void tip(float x, float y, float w, float h, std::string text);
    // The STEERING ROTATION section from `y`: the rotation, and that the wheel's own software must match it.
    void drawRotation(float y);
    // The Games page: the list of profiles, and the picked one's.
    void drawGames();
    // The Games list's bar was pressed (clickX_): starts dragging it, or on a second press fits the list.
    void pressListBar();
    // Writes the list's width to app.ini (games_list_width).
    void keepListWidth();
    // The picked game from `y` (x from gameX()): its name and whether it runs, then its three sections, each with what
    // its profile does and the user's values for it: MAPPING (the pedals the profile remaps, the steering rotation),
    // FORCE FEEDBACK (what it sends, while it runs), GAME CONFIG (the game's own settings and its settings file; left out when the profile
    // has none). No target: that is the Mapping tab's.
    float drawGameProfile(std::size_t index, float y);
    float drawGameSettings(std::size_t index, float y);
    // The sub-tabs' own parts: one AXES section (steering rotation; each pedal's axis the game reads and its
    // direction; Axes), the force feedback the game sends while it runs (Ffb, shown only), the buttons (Buttons); each value with Default (back to where it
    // came from: the game's profile or the Mapping tab).
    float drawGameAxes(std::size_t index, float y);
    float drawGameForces(std::size_t index, float y);
    float drawGameButtons(std::size_t index, float y);
    // The picked game's own values (created when one is set; dropped when none is left).
    [[nodiscard]] const GameTuning* tuningOf(const std::string& id) const;
    GameTuning& tuningFor(const std::string& id);
    void dropEmptyTuning(const std::string& id);
    [[nodiscard]] bool tuningChanged(const std::string& id) const;
    // Add game...: the system's file window for the game's .exe (starting in `start`); a game WheelBri does not know
    // becomes the user's (games.ini) and its folder joins the shim folders (no shim put in).
    void addGame(const std::filesystem::path& start = {});
    // The picked game's FOLDER section (Games page): its folder, the shim there and what to do with it; `game` null
    // for a folder whose game WheelBri does not know. Returns the y below it.
    float drawGameFolder(const GameProfile* game, std::optional<std::size_t> folder, float y);
    // Which listed folder holds each game (by id) and which folders hold none (read again every 2 s at most).
    void refreshGameFolders();
    // Remove: a game the user added (and its own values); its folder stays listed.
    void removeGame(const std::string& id);
    void reloadGames();
    [[nodiscard]] bool userGame(const std::string& id) const;
    // A picker's source for what is picked, and setting it (GameButton: the picked game's own value).
    [[nodiscard]] SourceRef pickedSource() const;
    void setPicked(const SourceRef& source);
    void gameSection(float y, const std::string& name, const std::string& about);
    // A game's settings file: the one the user located (Locate: kept in the app state as game_file.<id>; not when
    // the game was seen writing another), else the one the game was seen writing (game_file_seen.<id>), else the
    // profile's under Documents. As root + a relative
    // path, for GameSettings' checks (no link, nothing outside root). `confirmed`: the game was seen writing it, or it
    // is where the game's profile says (settingsFileFits); otherwise Save to file asks first (it may be another
    // game's: NFS Heat's file holds NFS Unbound's settings too).
    struct GameFilePlace {
        std::filesystem::path root;
        std::string relative;
        bool located = false;
        bool confirmed = true;
    };
    [[nodiscard]] GameFilePlace gameFilePlace(const GameProfile& game) const;
    // Its values as the tab shows them (saved or not), as the game writes them.
    [[nodiscard]] std::vector<GameSettingText> gameValues(const GameProfile& game) const;
    // Locate: the system's Open dialog; a file that holds every one of the game's settings is remembered (one the game
    // was not seen writing too, not confirmed).
    void locateGameFile(std::size_t index);
    // Save to file: the values into the game's file (never while it runs), the original kept once. A file not
    // confirmed is written on the second press only.
    void saveGameFile(std::size_t index);
    // The game `id` ended (it ran since `since`): the settings file it wrote then is its own (game_file_seen.<id>).
    void learnGameFile(const std::string& id, std::filesystem::file_time_type since);
    [[nodiscard]] std::size_t selectedGameIndex() const;
    // A number field showing `value` (or what is typed in it); amber when it differs from the saved mapping.
    // The target's centring spring (G25, G27), below the rotation; returns where it ends.
    float drawCentringSpring(float top);
    void drawField(float x, float y, float w, Field field, const std::string& value, bool changed,
                   std::size_t index = 0);
    [[nodiscard]] std::string fieldText(Field field, std::size_t index) const;
    void startTyping(Field field, std::size_t index = 0);
    // Applies what was typed: false (and the reason in the status) when it is not a usable value.
    bool commitTyping();
    void setRotationFromX(float x);
    // The secondary popup's axis being dragged (altFocus_): its threshold from where the pointer is on its row's
    // slider (whole percent).
    void setThresholdFromX(float x);
    // The picked button's secondary `index` when it is an axis, or null.
    [[nodiscard]] ButtonRef* axisSecondary(std::size_t index);
    float button(float x, float y, const std::string& label, std::function<void()> action, bool on = false,
                 bool enabled = true, float width = 0);
    void hit(float x, float y, float w, float h, std::function<void()> action);
    // System's members, or the system's own.
    [[nodiscard]] std::optional<std::filesystem::path> pickFile(const std::string& title,
                                                                const std::filesystem::path& folder,
                                                                const std::string& fileName) const;
    [[nodiscard]] bool canPickFile() const;
    [[nodiscard]] std::filesystem::path documentsFolder() const;
    [[nodiscard]] std::optional<std::vector<RunningProgram>> runningPrograms() const;
    [[nodiscard]] std::uint64_t nowMs() const;

    ui::UiWindow& window_;
    System system_;
    std::filesystem::path rigsFolder_;
    std::filesystem::path stateFile_;
    std::string rigName_;
    Rig rig_;
    std::vector<std::string> rigNotices_;   // what the Rig could not map (followRig), said in the status line
    ConfigPicker& picker_;
    std::vector<Hit> hits_;
    std::vector<Tip> tips_;
    std::size_t popupTips_ = 0;   // tips_ from here on are the open popup's
    float hoverX_ = 0.0F;
    float hoverY_ = 0.0F;
    bool hoverInside_ = false;
    std::size_t altFocus_ = 0;   // the secondary whose threshold the picker edits (when it is an axis)
    std::string altGameId_;      // the game whose list the secondary picker edits (altForGame)
    // The Buttons sub-tab's rows added by Add button... and not changed yet (shown so their main and secondaries can
    // be changed), for pendingKey_ (the game and the target); dropped when another game or target shows.
    std::vector<std::size_t> pendingButtons_;
    std::string pendingKey_;
    std::size_t pendingFlash_ = 0;            // the row just added: outlined blue until pendingFlashUntilMs_
    std::uint64_t pendingFlashUntilMs_ = 0;
    // The pickers (Pick, PickAlt): the find box (only entries holding it are listed), how far the list is scrolled
    // down (logical pixels) and at most (from the last draw).
    std::string findText_;
    float listScroll_ = 0;
    float listScrollMax_ = 0;
    // The last control touched on the rig while a picker was open: its entry flashes until touchFlashUntilMs_;
    // touchNote_ says what happened (added, or why not), in green or amber, until touchNoteUntilMs_.
    SourceRef touchFlash_{};
    bool touchFlashAxis_ = false;
    std::uint64_t touchFlashUntilMs_ = 0;
    std::string touchNote_;
    bool touchNoteOk_ = true;
    std::uint64_t touchNoteUntilMs_ = 0;

    std::uint64_t seenChanges_ = ~0ULL;
    std::string path_;
    bool builtIn_ = false;
    std::string fileText_;
    bool loaded_ = false;
    std::unique_ptr<IDeviceProfile> profile_;
    MappingModel saved_;
    MappingModel edit_;
    static constexpr std::size_t kMaxUndo = 50;
    std::vector<MappingModel> undo_;           // the saved mappings before each change, oldest first
    int undoTarget_ = -1;                      // the Target they are for (another Target: the list starts again)
    std::vector<std::size_t> renumbered_;      // readRenumbered: rows marked "number changed", ascending
    bool undoing_ = false;                     // the next save is an Undo's: nothing goes on the list
    std::optional<MappingModel> notSaved_;     // the change that could not be saved (not tried again)
    std::vector<DeviceLabels> devices_;
    std::uint64_t devicesReadMs_ = 0;

    Mode mode_ = Mode::Idle;
    Slot pickKind_ = Slot::Axis;
    std::size_t pickIndex_ = 0;
    bool locked_ = false;
    std::string sessionPath_;   // the config the running session was started with (path_ at setLocked(true))
    std::string status_;
    std::string toast_;
    bool statusError_ = false;

    std::string targetName_ = "g920";   // [emulator] target_profile: which games' advice fits
    Field field_ = Field::None;
    std::size_t fieldIndex_ = 0;
    std::string typed_;
    bool dragging_ = false;   // the slider, from a press on it until the left button is let go
    bool thresholdDragging_ = false;   // the secondary popup's threshold slider, the same way
    float thresholdSliderX_ = 0;       // where the axis rows' sliders were drawn last (their pointer maths)
    float thresholdSliderW_ = 1;
    // The Games list's width: as the user left it (app.ini games_list_width, read at the first draw of the page);
    // dragged by its bar from a press on it until the left button is let go, then kept; a second press within
    // kDoubleClickMs fits the list to its longest name and tag.
    static constexpr std::uint64_t kDoubleClickMs = 400;
    float gameListW_ = kGameListMinW;
    bool gameListRead_ = false;
    bool listDragging_ = false;
    float listGrab_ = 0.0F;            // the press's distance from the list's right edge
    std::uint64_t gripPressMs_ = 0;    // the last press on the bar
    float fitListW_ = kGameListMinW;   // what the longest name and tag need (the last draw's)
    float clickX_ = 0.0F;     // where the click being answered is (the slider's press)

    Page page_ = Page::Mapping;
    ShimGamesView* shims_ = nullptr;
    std::map<std::string, std::size_t> gameFolders_;   // game id -> its folder's index in shims_->folders()
    std::vector<std::size_t> otherFolders_;            // folders with no known game in them
    std::uint64_t gameFoldersReadMs_ = 0;
    std::size_t gameFoldersCount_ = 0;
    // The Mapping page's button groups shown although nothing in them is mapped (their Show), until the app closes.
    std::vector<std::string> shownGroups_;
    std::filesystem::path selectedFolderOnly_;         // the Games page shows that folder (its game is not known)
    std::string selectedGame_;   // the Games page's game (a profile id); empty: the first
    GameTab gameTab_ = GameTab::Axes;
    // The games the page lists: the built-in profiles, then the ones the user added (games.ini, userGames_).
    std::vector<GameProfile> games_;
    std::vector<GameProfile> userGames_;
    GameNow now_;
    // The picked game's settings file as last read (at most every 2 s, and when the game or the saved values change).
    std::string watchedGame_;                           // the known game running (GameNow::id), to see it end
    std::filesystem::file_time_type watchedSince_{};    // since when it runs (file times), a little before
    std::string saveAnyway_;                            // the file not confirmed that Save to file was pressed for
    std::optional<GameSettingsFileState> fileState_;
    std::string fileStateKey_;
    std::uint64_t fileStateReadMs_ = 0;
};

}  // namespace wheelbri::service
