#pragma once

#include "wheelbri/EmulatedJoystick.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wheelbri {

// The games the app put the in-game shim into (Mode Shim): each is a folder holding the game's .exe,
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
// The shim's name next to the Ultimate ASI Loader (ThirteenAG's, the dinput8.dll of the Widescreen Fixes Pack): the
// loader takes the game's DirectInput calls and hands them to <its name>Hooked.dll when that file is there, in place
// of Windows' dinput8.dll; the shim then goes on to Windows' own, as always.
inline constexpr const char* kShimDllHooked = "dinput8Hooked.dll";

// Which processor a Windows program or DLL is built for, from its PE header.
enum class Architecture { Unknown, X86, X64 };
[[nodiscard]] Architecture peArchitecture(const std::filesystem::path& file);
// A game folder's: that of its largest .exe (the game itself; launchers and tools are smaller). Unknown when no .exe
// can be read.
[[nodiscard]] Architecture gameArchitecture(const std::filesystem::path& folder);

// Whether `dll` is the Ultimate ASI Loader (it exports IsUltimateASILoader).
[[nodiscard]] bool isAsiLoader(const std::filesystem::path& dll);
// Where the shim is (or goes) in `folder`: dinput8Hooked.dll when its dinput8.dll is the Ultimate ASI Loader,
// else dinput8.dll.
[[nodiscard]] std::filesystem::path shimDllIn(const std::filesystem::path& folder);

// WheelBri's two builds of the shim: a 32-bit game loads only a 32-bit DLL, a 64-bit game only a 64-bit one.
struct ShimSources {
    std::filesystem::path x64;   // shim\dinput8.dll (build/shim)
    std::filesystem::path x86;   // shim\x86\dinput8.dll (build-x86/shim); empty when not built
};
// The one for the game in `folder` (gameArchitecture; x64 when it cannot tell).
[[nodiscard]] std::filesystem::path shimSourceFor(const std::filesystem::path& folder, const ShimSources& sources);

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

// Of the shim's file in the folder (shimDllIn: dinput8.dll, or dinput8Hooked.dll next to the ASI Loader).
enum class ShimState {
    Installed,   // our dinput8.dll is there, the same as the one WheelBri has
    Outdated,    // ours, but WheelBri has a newer one (Start replaces it); also ours built for the other processor
    Missing,     // no dinput8.dll
    Foreign,     // a dinput8.dll that is not ours (another mod): left alone
};

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
// file's other lines (the user's edits, comments, CRLF, a BOM) kept. Nothing is written when it already says so
// (`changed` stays empty); a missing file is created with just that line. Fails (nothing written) for a profile that is
// not a target, a missing folder, a folder in the file's place, an unreadable file. The app does it at Start, for the
// target picked on the Mapping tab.
ShimResult setShimTarget(const std::filesystem::path& folder, const std::string& profile);
// The pedal axes WheelBri lays out for the game in `folder` (its profile's, with the user's own axes for it: the Game
// profiles tab, GameTuning.hpp), as [shim] throttle_axis / brake_axis / clutch_axis with game_axes = wheelbri, which
// tells the shim to use them as they are (not lay the game's profile over them again). nullopt takes those lines out
// again, but only when game_axes = wheelbri says WheelBri wrote them: axes the user wrote are never touched. Refused
// for a layout the shim would refuse (isValid). Nothing is written when the file says it already. The app does it at Start.
ShimResult setShimGameAxes(const std::filesystem::path& folder, const std::optional<JoystickLayout>& axes);
// The game whose program is in `folder` (the first of `games`), or null.
[[nodiscard]] const GameProfile* gameInFolder(const std::filesystem::path& folder, std::span<const GameProfile> games);
// The key setShimGameAxes writes with the axes, and its value.
inline constexpr const char* kShimGameAxesKey = "game_axes";
inline constexpr const char* kShimGameAxesByWheelBri = "wheelbri";

// Deletes what installShim put there: dinput8.dll (only ours) and wheelbri-shim.ini.
ShimResult removeShim(const std::filesystem::path& folder, const std::filesystem::path& sourceDll);

}  // namespace wheelbri
