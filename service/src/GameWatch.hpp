#pragma once

#include "wheelbri/GameDetector.hpp"
#include "wheelbri/WayNotices.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTuning.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::service {

// Game profiles (GameProfiles.hpp) at work in a session: looks at the running programs (core GameDetector over
// platform::runningPrograms) for the few games that have a profile, says in the log when one is in use and what it
// changes, and gives the window a short note. A game without a profile is never named. With the virtual G920 (or
// wheelbri-uhid) it routes the published pedals as the profile says; with the in-game shim the shim applies the
// profile itself, so the pedals go out as they are and the log says so. With NFS 2015 followed it also reads the
// game's own wheel bindings (core Nfs2015Settings) whenever its settings file changes, and warns only when they would
// undo the profile. A game with a steering rotation (its profile's [steering] rotation_deg, or the user's
// [game_rotation] line; GameRotation.hpp) is played with it: the axis goes through 1:1 either way, so the log says
// the wheel's own software must be set to it while the game runs, and the window shows it.
// A game's own settings (its profile's [game_settings]) are never
// written here: the user saves them into the game's file from the Games tab (Save to file).
// For the app's Games tab it also says which game runs (live: profile id, pid, since when, how many starts).
// Called from the session's window thread every 2 s (a few milliseconds; never on the service loop); not thread-safe.
// The games the user added (GameTuning.hpp, parseUserGames) from games.ini in the user's settings folder; its problems
// are logged. Empty when there is no file.
[[nodiscard]] std::vector<GameProfile> loadUserGames();

class GameWatch {
public:
    // `routePedals`: the session publishes to the virtual G920 or wheelbri-uhid (the service applies profiles); false
    // for the in-game shim (the shim does). `mapping`: the session's own rotation in degrees ([steering]);
    // `overrides`: the user's per-game rotations ([game_rotation]). Logs the built-in profiles' problems, if any.
    // `session`: the target, the way and the user's per-game values (GameTuning.hpp);
    // `userGames`: the games the user added (games.ini), followed like the built-in profiles.
    // What it asks the system: the running programs, a process's program file and the Documents folder (platform::).
    // Each one left empty is the system's own; the tests give their own.
    struct System {
        std::function<std::optional<std::vector<RunningProgram>>()> runningPrograms;
        std::function<std::filesystem::path(std::uint32_t pid)> processImage;
        std::function<std::filesystem::path()> documentsFolder;
    };
    GameWatch(GameSessionConfig session, float mapping, GameRotations overrides, std::vector<GameProfile> userGames,
              System system = {});

    struct Decision {
        PedalRoute route;           // Pipeline::setPedalRoute
        // The game whose own mapping (pedal invert / curve, buttons) the pipeline should use; empty: the mapping's.
        std::string mapperGame;
        const GameTuning* tuning = nullptr;   // its values (owned by this GameWatch), when mapperGame is set
    };
    // One look: what the pipeline should use now.
    Decision check();
    // The user changed the per-game values while the session runs (the Games tab saves them as they change):
    // the next check() uses them. `tunings`: [game_tuning]; `overrides`: [game_rotation].
    void setValues(GameTunings tunings, GameRotations overrides);

    // For the window's status, after "Running 1:23, ": the profile's label ("NFS 2015") while it is in use, else empty.
    [[nodiscard]] std::string status() const;
    // For the window, in red: what the player should fix ("<GAME> NEEDS THE VIRTUAL DEVICE", "NFS 2015: RESET ITS
    // WHEEL CONTROLS"), or empty.
    [[nodiscard]] const std::string& warning() const { return wayWarning_.empty() ? warning_ : wayWarning_; }
    // A game that does not work the way this session runs ([game] ways, e.g. virtual only): the advice while it
    // runs, and how many times such a game started this session (the app tells the user once per start).
    [[nodiscard]] const std::optional<WayAdvice>& wayAdvice() const { return wayNotices_.current(); }
    [[nodiscard]] std::uint64_t wayNotices() const { return wayNotices_.count(); }
    // For the window, after the label: the rotation the game steers with while it is not the mapping's own ("180
    // deg"), else empty: the wheel's own software must be set to it while the game runs.
    [[nodiscard]] std::string rotationStatus() const;

    // For the app's Games tab: the game followed now, while its profile is in use.
    struct Live {
        std::string id;              // its id (a built-in profile's or a game the user added); empty: none
        std::uint32_t pid = 0;
        std::uint64_t sinceMs = 0;   // platform::nowMs() when it started being followed
        std::uint64_t starts = 0;    // profiled games started (followed) this session: the tab follows each new one
        bool profileApplies = false; // its built-in profile is in use (it fits the target)
    };
    [[nodiscard]] Live live() const;

private:
    void logEvents(const GameDetector::Update& update);
    void checkNfs2015Bindings();
    // The rotation for the game followed now (nullopt: the mapping's own), said in the log when it changes.
    [[nodiscard]] std::optional<float> currentRotation() const;
    void sayRotation();
    // Whether the game followed now plays with the user's own values for it, said in the log when it changes.
    void sayTuning();
    [[nodiscard]] std::string describeFollowed() const;

    System system_;
    GameSessionConfig session_;
    std::vector<GameProfile> games_;   // the built-in profiles, then the user's games: what detector_ follows
    GameDetector detector_;
    std::string target_;
    bool routePedals_;
    float mapping_;
    GameRotations overrides_;
    std::optional<float> applied_;              // what the last check() said
    std::string tunedGame_;                     // the game whose own values were said in the log last
    std::uint64_t starts_ = 0;                                 // Live::starts
    std::uint64_t followedSinceMs_ = 0;                        // Live::sinceMs
    const GameProfile* unroutable_ = nullptr;   // a profile the device cannot apply, said once
    std::string warning_;
    WayNotices wayNotices_;          // the session runs the shim when it does not route the pedals (the shim does)
    std::string wayWarning_;         // wayNotices_.current()'s title, upper case, for warning()
    // NFS 2015's settings file: checked again only when its last write changes.
    std::optional<std::filesystem::file_time_type> settingsWritten_;
};

}  // namespace wheelbri::service
