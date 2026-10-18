#pragma once

#include "UiWindow.hpp"

#include "wheelbri/ShimGames.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The app's games with the in-game shim, shown on the Mapping tab only with Mode Shim:
// "Game <name> [Games (N)]". The button opens the games panel (over the window): every game with its folder and
// whether the shim is there; a click on a game picks the one Start uses, Put shim / Remove shim and Forget (off the
// list, files left) per game, and Add game
// opens the system's folder window, puts the shim into the folder chosen (installShim) and adds the game. A game whose
// shim was deleted stays in the list, marked "no shim"; Start puts it back. The list is shimGamesFile().
class ShimGamesView {
public:
    struct Settings {
        std::filesystem::path listFile;         // shimGamesFile()
        ShimSources sources;                    // WheelBri's dinput8.dll, 64- and 32-bit (shimSourcesPath)
        std::filesystem::path sourceSettings;   // config/wheelbri-shim.ini
        std::filesystem::path stateFile;        // app.ini: shim_hot_swap = on | off (empty: not kept)
    };

    ShimGamesView(ui::UiWindow& window, Settings settings);

    // The row at (x, y), 370 wide. `canChange` is false while a session runs or starts, or a popup is open.
    void draw(float x, float y, bool canChange);
    // A click on the row; true if it was on it.
    bool onClick(float x, float y);

    // The games panel: open, it takes every click and draws over the window (drawPanel, last).
    [[nodiscard]] bool panelOpen() const { return panelOpen_; }
    void closePanel() { panelOpen_ = false; }
    void drawPanel();
    // A click on it; outside it, the panel closes (as Close), and the click goes nowhere else.
    void onPanelClick(float x, float y);

    [[nodiscard]] bool hasGame() const { return !list_.selected.empty(); }
    [[nodiscard]] const std::filesystem::path& selectedFolder() const { return list_.selected; }
    [[nodiscard]] std::string selectedName() const;

    // Before Start: the selected game must have the shim, as current as WheelBri's (put in or replaced now), presenting
    // `profile` ("g920", "g25": its wheelbri-shim.ini's target_profile). False and `error` set when it cannot (no game,
    // the game holds the DLL, another mod's dinput8.dll).
    bool prepareStart(const std::string& profile, std::string& error);
    // Hot swap (the panel's switch, on unless turned off, kept in app.ini): the shims in the games' folders follow the
    // target picked on the Mapping tab (App calls swapShims when it changes).
    [[nodiscard]] bool hotSwap() const { return hotSwap_; }
    // Goes up when the list or a folder's shim changes here (add, put, remove, forget, pick) or hot swap is turned on:
    // the shims are swapped again then.
    [[nodiscard]] std::uint64_t changes() const { return changes_; }
    // Every listed game whose folder has WheelBri's shim in it (as current as WheelBri's, or older) gets `target`
    // (setShimTarget) and the pedal axes `axesFor` gives for its folder (setShimGameAxes). A listed game without the
    // shim in its folder, or with another mod's dinput8.dll, is not touched. Returns what could not be done, one line
    // per game ("Heat: cannot write ..."); the other games are swapped all the same.
    using AxesFor = std::function<std::optional<JoystickLayout>(const std::filesystem::path& folder)>;
    [[nodiscard]] std::vector<std::string> swapShims(const std::string& target, const AxesFor& axesFor);

    // Deletes the shim from `folder` (after a session, when the user says so). False and `error` set if it cannot.
    bool removeFrom(const std::filesystem::path& folder, std::string& error);

    // Something went wrong in Add (the app shows it in a popup): taken once.
    [[nodiscard]] bool takeProblem(std::string& problem);

private:
    struct Hit {
        float x, y, w, h;
        std::function<void()> action;
    };

    void add();
    void pick(std::size_t index);
    void put(std::size_t index);
    void remove(std::size_t index);
    void forget(std::size_t index);   // off the list only
    void save();
    void refreshState();
    void refreshStates();   // every game's, for the panel
    // WheelBri's dinput8.dll for the game in `folder` (32-bit for a 32-bit game).
    [[nodiscard]] std::filesystem::path source(const std::filesystem::path& folder) const;

    ui::UiWindow& window_;
    Settings settings_;
    ShimGameList list_;
    std::vector<Hit> hits_;
    std::vector<Hit> panelHits_;
    std::vector<ShimState> states_;          // per game, while the panel is open
    std::vector<std::string> targets_;       // per game, the wheel its shim presents (target_profile), while open
    bool hotSwap_ = true;
    std::uint64_t changes_ = 0;
    bool panelOpen_ = false;
    float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;   // the panel as last drawn: a click outside closes it
    ShimState state_ = ShimState::Missing;   // of the selected game
    std::uint64_t stateReadMs_ = 0;
    std::string problem_;
};

}  // namespace wheelbri::service
