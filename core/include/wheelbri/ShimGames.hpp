#pragma once

#include "wheelbri/FileText.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace wheelbri {

// The games the app put the in-game shim into (Run as Shim): each is a folder holding the game's .exe,
// where the shim is dinput8.dll plus its settings, wheelbri-shim.ini. The list lives in a small file of the app's
// (shimGamesFile() in the service), one game per line, so the user never types a path: Add game picks the folder.
struct ShimGame {
    std::string name;                  // shown in the app: the folder's name
    std::filesystem::path folder;

    bool operator==(const ShimGame&) const = default;
};

struct ShimGameList {
    std::vector<ShimGame> games;
    std::filesystem::path selected;    // the game Start uses (one of games' folders, or empty)
};

// The shim's files in a game folder.
inline constexpr const char* kShimDll = "dinput8.dll";
inline constexpr const char* kShimSettings = "wheelbri-shim.ini";

// Paths in the list file are UTF-8 (game folders often are not ASCII): pathToUtf8 / pathFromUtf8 (FileText.hpp).

// Reads the list (a missing file is an empty list). Lines it does not understand are skipped.
[[nodiscard]] ShimGameList readShimGames(const std::filesystem::path& file);
// Writes it, creating the folder. False if it could not.
bool writeShimGames(const std::filesystem::path& file, const ShimGameList& list);
// Adds `folder` (or finds it: same folder, any case) and selects it. Returns its index.
std::size_t addShimGame(ShimGameList& list, const std::filesystem::path& folder);
// Takes game `index` off the list (its folder is not touched). When it was the selected one, the first game left is
// selected (or none). False if there is no such game.
bool removeShimGame(ShimGameList& list, std::size_t index);

// Why `folder` cannot be a game folder ("" if it can): it must exist and hold at least one .exe.
[[nodiscard]] std::string gameFolderProblem(const std::filesystem::path& folder);

enum class ShimState {
    Installed,   // our dinput8.dll is there, the same as the one WheelBri has
    Outdated,    // ours, but WheelBri has a newer one (Start replaces it)
    Missing,     // no dinput8.dll
    Foreign,     // a dinput8.dll that is not ours (another mod): left alone
};
[[nodiscard]] const char* shimStateName(ShimState state);

// What is in `folder`, compared with WheelBri's own dinput8.dll at `sourceDll` (when that is missing, any
// dinput8.dll next to a wheelbri-shim.ini counts as ours and installed). A dinput8.dll is ours when it is the same file
// as `sourceDll` or has wheelbri-shim.ini next to it.
[[nodiscard]] ShimState shimState(const std::filesystem::path& folder, const std::filesystem::path& sourceDll);

struct ShimResult {
    bool ok = false;
    std::string error;                 // why not, for the window and the log
    std::vector<std::string> changed;  // the files written or deleted
};

// Puts the shim into `folder`: copies `sourceDll` (replacing ours, never a foreign one) and `sourceSettings` when no
// wheelbri-shim.ini is there yet (the user's edits stay). Fails while the game runs (Windows locks the DLL).
ShimResult installShim(const std::filesystem::path& folder, const std::filesystem::path& sourceDll,
                       const std::filesystem::path& sourceSettings);
// Makes the shim in `folder` present `profile` ("g920", "g25"): [shim] target_profile in its wheelbri-shim.ini, the
// file's other lines (the user's edits) kept. Nothing is written when it already says so (`changed` stays empty); a
// missing file is created with just that line. The app does it at Start, for the target picked on the Mapping tab.
ShimResult setShimTarget(const std::filesystem::path& folder, const std::string& profile);
// Deletes what installShim put there: dinput8.dll (only ours) and wheelbri-shim.ini.
ShimResult removeShim(const std::filesystem::path& folder, const std::filesystem::path& sourceDll);

}  // namespace wheelbri
