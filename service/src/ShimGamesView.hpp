#pragma once

#include "HitList.hpp"
#include "UiWindow.hpp"

#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/ShimGames.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <utility>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// The game folders the in-game shim goes into (shimGamesFile()): each folder's shim (put in, older, none, another
// mod's) and the wheel it presents, and what is done to them (put in, deleted, forgotten, the hot swap, Start). There
// is no game to pick: Start readies every listed folder, and the app sees which game runs. The Games tab
// (MappingEditView's Games page) shows and changes them per game; the Mapping tab shows them in a row, "Game <names>
// [Games]", whose button opens the Games tab (Settings::openGames). A game whose shim was deleted stays in the list,
// marked "no shim"; Start puts it back. (The list file's "selected" line, from older versions, is kept but not used.)
class ShimGamesView {
public:
    struct Settings {
        std::filesystem::path listFile;         // shimGamesFile()
        ShimSources sources;                    // WheelBri's dinput8.dll, 64- and 32-bit (shimSourcesPath)
        std::filesystem::path sourceSettings;   // config/wheelbri-shim.ini
        std::filesystem::path stateFile;        // app.ini: shim_hot_swap = on | off (empty: not kept)
        // The name of the game in a folder when WheelBri knows it ("NFS Unbound" for its Excalibur folder), else
        // empty: the folder's name is shown. Null: folder names only.
        std::function<std::string(const std::filesystem::path& folder)> gameLabel;
        // The row's Games button: the app opens the Games tab.
        std::function<void()> openGames;
    };

    ShimGamesView(ui::UiWindow& window, Settings settings);

    // The row at (x, y), 370 wide: the listed games. `canChange` is false while a session runs or starts, or a popup
    // is open.
    void draw(float x, float y, bool canChange);
    // A click on the row; true if it was on it.
    bool onClick(float x, float y);

    // The folders, in the list's order; and each one's shim and the wheel it presents (read again when older than
    // 2 s, or now with refresh(true)).
    [[nodiscard]] const std::vector<ShimGame>& folders() const { return list_.games; }
    void refresh(bool now = false);
    [[nodiscard]] ShimState stateAt(std::size_t index) const;
    [[nodiscard]] std::string targetAt(std::size_t index) const;   // its target_profile ("g923"), "" without one
    // Its shim is next to the Ultimate ASI Loader (dinput8Hooked.dll); chained to another mod's DLL (shimChained);
    // why another mod's DLL there cannot be chained ("" when it can, or there is none: chainProblem).
    [[nodiscard]] bool asiLoaderAt(std::size_t index) const;
    [[nodiscard]] bool chainedAt(std::size_t index) const;
    [[nodiscard]] std::string chainProblemAt(std::size_t index) const;
    // What the Games tab does with a folder: put the shim in, delete it, take the folder off the list (files left).
    // What goes wrong is a problem (takeProblem), what is done a short message (takeToast).
    void put(std::size_t index);
    void remove(std::size_t index);
    void forget(std::size_t index);
    // The system's folder window for the folder of `game` (null: any game's): it must hold an .exe, and `game`'s
    // program when given. Added to the list (and picked when nothing is), no shim put in yet. False when cancelled or
    // refused (the problem says why).
    bool chooseFolder(const GameProfile* game, const std::filesystem::path& start);
    // Adds `folder` as it is (an .exe there); false and `problem` set otherwise. No shim put in.
    bool addFolder(const std::filesystem::path& folder, std::string& problem);
    void setHotSwap(bool on);

    // Mode Shim needs at least one game listed (Start stays greyed out without).
    [[nodiscard]] bool hasGame() const { return !list_.games.empty(); }

    using AxesFor = std::function<std::optional<JoystickLayout>(const std::filesystem::path& folder)>;
    // Before Start: every listed folder gets WheelBri's shim, as current as WheelBri's (put in, replaced by a newer
    // one, or put back where it was deleted), presenting `profile` ("g920", "g25": target_profile in its
    // wheelbri-shim.ini) with the pedal axes `axesFor` gives for it. There is no game to pick: whichever of them starts
    // finds its shim ready. A folder with another mod's dinput8.dll is left alone (said in the log). `prepared`: the
    // folders done. False and `error` set when one cannot be done, or none is left (no game, only other mods' DLLs).
    bool prepareStart(const std::string& profile, const AxesFor& axesFor, std::vector<std::filesystem::path>& prepared,
                      std::string& error);

    // What Stop does with the shims (the "delete the shim?" question; its "Don't ask again" keeps the answer in
    // app.ini, shim_at_stop = ask | keep | delete; the Games tab's Ask again puts the question back).
    enum class AtStop { Ask, Keep, Delete };
    [[nodiscard]] AtStop atStop() const { return atStop_; }
    void setAtStop(AtStop atStop);
    // Hot swap (the Games tab's switch, on unless turned off, kept in app.ini): the shims in the games' folders follow
    // the target picked on the Mapping tab (App calls swapShims when it changes).
    [[nodiscard]] bool hotSwap() const { return hotSwap_; }
    // Goes up when the list or a folder's shim changes here (add, put, remove, forget, pick) or hot swap is turned on:
    // the shims are swapped again then.
    [[nodiscard]] std::uint64_t changes() const { return changes_; }
    // Every listed game whose folder has WheelBri's shim in it (as current as WheelBri's, or older) gets `target`
    // (setShimTarget) and the pedal axes `axesFor` gives for its folder (setShimGameAxes). A listed game without the
    // shim in its folder, or with another mod's dinput8.dll, is not touched. Returns what could not be done, one line
    // per game ("Heat: cannot write ..."); the other games are swapped all the same.
    [[nodiscard]] std::vector<std::string> swapShims(const std::string& target, const AxesFor& axesFor);

    // Deletes the shim from `folder` (after a session, when the user says so). False and `error` set if it cannot.
    bool removeFrom(const std::filesystem::path& folder, std::string& error);

    // How the game is shown: its name when WheelBri knows the game in the folder, with the folder's name after it when
    // that differs ("NFS Unbound (Excalibur)"); else the folder's name.
    [[nodiscard]] std::string displayName(const ShimGame& game) const;
    // The name of the game WheelBri knows in `folder` (gameLabel, looked up every few s), or empty.
    [[nodiscard]] std::string labelOf(const std::filesystem::path& folder) const;
    // A short message after something was done here ("Shim put into NFS Unbound"): taken once, shown for a few s.
    [[nodiscard]] std::string takeToast() { return std::exchange(toast_, std::string()); }

    // Something went wrong in Add (the app shows it in a popup): taken once.
    [[nodiscard]] bool takeProblem(std::string& problem);

private:
    void save();
    void refreshStates();   // every folder's
    // WheelBri's dinput8.dll for the game in `folder` (32-bit for a 32-bit game).
    [[nodiscard]] std::filesystem::path source(const std::filesystem::path& folder) const;

    ui::UiWindow& window_;
    Settings settings_;
    ShimGameList list_;
    HitList hits_;
    std::vector<ShimState> states_;          // per folder
    std::vector<std::string> targets_;       // per folder, the wheel its shim presents (target_profile)
    std::vector<bool> asiLoaders_;           // per folder: asiLoaderAt, chainedAt, chainProblemAt
    std::vector<bool> chained_;
    std::vector<std::string> chainProblems_;
    std::uint64_t statesReadMs_ = 0;
    bool hotSwap_ = true;
    std::uint64_t changes_ = 0;
    AtStop atStop_ = AtStop::Ask;
    std::string problem_;
    std::string toast_;
    mutable std::map<std::filesystem::path, std::string> labels_;   // gameLabel's answers, read again every few s
    mutable std::uint64_t labelsReadMs_ = 0;
};

}  // namespace wheelbri::service
