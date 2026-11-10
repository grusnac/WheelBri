// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatedJoystick.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// Game profiles: the exception, not the rule. WheelBri's defaults (the target's own layout, a real G920's: throttle
// Y, brake Rz, clutch Slider) are the base for every game. A game that reads the wheel differently gets a profile
// file, config/games/<id>.ini, that holds ONLY what that game needs changed, and it is laid over the defaults like a
// sieve: a key set in the profile replaces the default, a key left out keeps it. The files are compiled in (core's
// CMakeLists.txt embeds every config/games/*.ini), so nothing is installed or set; a game without a file is never
// named. The same profile serves both ways to the game:
//  - the G920 virtual device (and wheelbri-uhid): wheelbri-service finds the game among the running programs
//    (GameDetector) and routes the published pedals so the device's fixed axes carry what the game reads there
//    (routePedals, Pipeline::setPedalRoute);
//  - the G920 in-game shim: the shim knows the game it is loaded into and lays the profile over its own layout
//    (ShimConfig: defaults, then wheelbri-shim.ini, then the profile); the service leaves the pedals alone.
//
// A profile file:
//   [game]   label = NFS 2015 / program = nfs16.exe / target = g920   (who it is for; all three required)
//            target = any: every target, for a profile that sets no pedal axes (they are one target's).
//            label: what the window and the log show, short (at most 20 characters); Need for Speed games are
//            "NFS <name>" (NFS 2015, NFS Heat), never "Need for Speed ...".
//            ways = virtual / ways_note = why   (both or neither): the ways the game works with, virtual (the virtual
//            device) and/or shim (the in-game shim), comma-separated; left out, both. A session run the other way
//            tells the user when the game starts (wayAdvice).
//   [pedals] throttle_axis / brake_axis / clutch_axis = x y z rx ry rz slider none   (only those that change)
//            combined = true: Logitech's "Report Combined Pedals" while the game runs (withCombinedPedals,
//            DeviceProfile.hpp), for games that read throttle and brake from one axis; the user's [game_tuning]
//            <game>.combined_pedals overrides it.
//   [steering] rotation_deg = 180 / note = why   (both or neither): a game whose own handling needs another
//            steering rotation than usual (NFS Payback's cars understeer and turn in slowly). While the game runs the
//            service steers with it, wheel and game both at that number (GameRotation.hpp), unless the user's mapping
//            says otherwise for that game ([game_rotation], set in the Games tab). WheelBri never reads or
//            sets the wheel's own software: the log and the window say to set it to the same number.
//   [game_settings] file = <path under the user's Documents folder> / note = why / <id> = <the game's name> <value>
//            (file, note and at least one setting go together; at most kMaxGameSettings): the game's own settings that
//            the user writes into the game's settings file from the Games tab (GameSettings.hpp; NFS Unbound
//            keeps its wheel force feedback settings there and shows none of them in its menus). <id> is lower case
//            (spring), the user's name for it in the Games tab and in [game_settings] <game>.<id> = value;
//            <value> is a number from kGameSettingMin to kGameSettingMax, or "steering" for the steering rotation the
//            game is played with (not changed by the user here: it follows the mapping's rotation for that game).
struct GameSetting {
    std::string id;                  // spring
    std::string name;                // the game's own name for it: WheelSpringMagnitude
    std::optional<float> value;      // nullopt: the steering rotation
    friend bool operator==(const GameSetting&, const GameSetting&) = default;
};
inline constexpr std::size_t kMaxGameSettings = 8;
inline constexpr float kGameSettingMin = 0.0F;
inline constexpr float kGameSettingMax = 1.0F;

struct GameProfile {
    std::string id;        // the file's name without .ini: "nfs2015"
    std::string label;     // [game] label, shown as it is in the window's status and the log: "NFS 2015"
    std::string program;   // the game's program file, lower case: "nfs16.exe"
    // A game the user added: its program's full path, as picked ("D:\Games\NFS MW\speed.exe"), so two games whose
    // programs share a name stay apart. Empty: found by its program's name alone (built-in profiles, older games.ini).
    std::string path;
    std::string target;   // the target profile it was checked with, lower case: "g920"; empty: every target
    bool virtualDevice = true;   // [game] ways: it works with the virtual device
    bool shim = true;            //              and with the in-game shim
    std::string waysNote;        // [game] ways_note: why not the other way, in a sentence (set with ways only)
    // [game] not_for = g923: targets the game does not support (it reads them as some other wheel); not_for_note: what
    // goes wrong then, in a sentence. Set together; checked in the game, not guessed.
    std::vector<std::string> notFor;
    std::string notForNote;
    std::optional<ObjectGuid> throttleAxis;   // [pedals]: unset = the default
    std::optional<ObjectGuid> brakeAxis;
    std::optional<ObjectGuid> clutchAxis;
    std::optional<bool> combinedPedals;   // [pedals] combined: Logitech's "Report Combined Pedals" while it runs
    std::optional<float> rotationDeg;   // [steering] rotation_deg: the rotation while the game runs (wheel = game)
    std::string rotationNote;           // [steering] note: why, in a sentence, for the user (the log says it)
    std::string settingsFile;           // [game_settings] file: relative to the user's Documents folder, / separated
    std::string settingsNote;           // [game_settings] note
    std::vector<GameSetting> settings;  // [game_settings] the rest, by id
};



// One built-in profile file as compiled in: its id and its text.
struct GameProfileFile {
    std::string_view id;
    std::string_view text;
};
[[nodiscard]] std::span<const GameProfileFile> builtInGameProfileFiles();

struct ParsedGameProfile {
    std::optional<GameProfile> profile;   // nullopt when the file has problems: then it is not used at all
    std::vector<std::string> problems;    // every problem found, not only the first
};
[[nodiscard]] ParsedGameProfile parseGameProfile(std::string_view id, std::string_view text);

// The built-in profiles that parsed cleanly, parsed once (thread-safe). A file with problems is left out; its problems
// are in gameProfileProblems() ("nfs2015.ini: ..."), which the service and the shim log.
[[nodiscard]] std::span<const GameProfile> gameProfiles();
[[nodiscard]] const std::vector<std::string>& gameProfileProblems();

// The program's file name, lower case, from what the system says about a process: a bare name ("NFS16.exe") or a
// path with / or \ separators (a Wine process's command line on Linux: "Z:\games\NFS16.exe"). Only ASCII letters are
// lowered. Empty for empty input or a path ending in a separator.
[[nodiscard]] std::string programName(std::string_view nameOrPath);

// The profile for a program, or null. Exact file-name match: "nfs16.exe.bak" or "notnfs16.exe" are not NFS 2015.
[[nodiscard]] const GameProfile* gameProfileFor(std::string_view nameOrPath,
                                                std::span<const GameProfile> profiles = gameProfiles());

// What to tell the user when `game` starts in a session run a way it does not work with (`shim`: the in-game shim; else
// the virtual device): a short title for the notification ("NFS Payback needs Mode Virtual") and what to do;
// nullopt when the game works that way.
struct WayAdvice {
    std::string title;
    std::string text;
    friend bool operator==(const WayAdvice&, const WayAdvice&) = default;
};
[[nodiscard]] std::optional<WayAdvice> wayAdvice(const GameProfile& game, bool shim);

// What to tell the user when `game` is played as `target` (the Mapping tab's Target, "g923") and it does not support
// that wheel ([game] not_for): "NFS Unbound does not support the G923: ... Use the G920 for it (Mapping tab, Target)."
// nullopt when it does (or nothing is known against it).
[[nodiscard]] std::optional<std::string> targetAdvice(const GameProfile& game, std::string_view target);

// True when the profile is for this target profile (any case), or for every target (a game the user added, whose
// target is empty: GameTuning.hpp).
[[nodiscard]] bool profileFitsTarget(const GameProfile& profile, std::string_view targetProfile);

// The sieve: `base` with every pedal axis the profile sets put in its place; everything else as in `base`.
[[nodiscard]] JoystickLayout overlay(JoystickLayout base, const GameProfile& profile);
// "throttle_axis = slider, brake_axis = y, clutch_axis = rz": what the profile changes, for the log; empty: nothing.
[[nodiscard]] std::string describeOverlay(const GameProfile& profile);

// For a device whose pedals are packed at fixed axes (the virtual wheel: TargetReport throttle on Y, brake on Rz,
// clutch on the Slider): which pedal each report field carries. from[0] feeds the throttle field, from[1] the brake
// field, from[2] the clutch field; 0 = throttle, 1 = brake, 2 = clutch.
struct PedalRoute {
    std::array<std::uint8_t, 3> from{0, 1, 2};
    [[nodiscard]] bool identity() const { return from[0] == 0 && from[1] == 1 && from[2] == 2; }
    friend bool operator==(const PedalRoute&, const PedalRoute&) = default;
};

// The route that makes a device laid out as `device` look like `game` to the game: every axis the game reads a pedal
// from must be one of the device's pedal axes, and every device pedal axis must carry one (a permutation). nullopt
// otherwise: the device cannot do it (e.g. a profile putting a pedal on Z for the virtual wheel, which has no Z).
[[nodiscard]] std::optional<PedalRoute> routePedals(const JoystickLayout& device, const JoystickLayout& game);

// `report` with its pedal fields fed as `route` says. Allocation-free (the service loop).
[[nodiscard]] TargetReport applyPedalRoute(const TargetReport& report, PedalRoute route);

// A process as the system lists it: its id and a name or path.
struct RunningProgram {
    std::uint32_t pid = 0;
    std::string name;
    std::string path = {};   // its program's full path when known (asked only for games with a path); else empty
};

// Whether two Windows paths name the same file as written: ASCII case and / or \ aside.
[[nodiscard]] bool samePath(std::string_view a, std::string_view b);

// A game with a profile found running: its profile and its process.
struct RunningGame {
    const GameProfile* game = nullptr;
    std::uint32_t pid = 0;
    std::string program;   // the name or path as listed
};

// The profiled games among `programs`, in profile order, once per game (its first process), each process one game's
// at most. A game with a path takes only a process running from there (or one whose path is not known); those go
// first, so a game found by name alone never takes their process.
[[nodiscard]] std::vector<RunningGame> findRunningGames(std::span<const RunningProgram> programs,
                                                        std::span<const GameProfile> profiles = gameProfiles());

}  // namespace wheelbri
