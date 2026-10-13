#pragma once

#include "wheelbri/GameDetector.hpp"
#include "wheelbri/WayNotices.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

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
// A game with a force feedback gain (its profile's [ffb] gain: NFS Unbound) gets it while it runs: check() returns it
// for the service's FfbController, and the log says so. A game's own settings (its profile's [game_settings]) are never
// written here: the user saves them into the game's file from the Game profiles tab (Save to file).
// For the app's Game profiles tab it also says which game runs (live: profile id, pid, since when, how many starts).
// Called from the session's window thread every 2 s (a few milliseconds; never on the service loop); not thread-safe.
class GameWatch {
public:
    // `routePedals`: the session publishes to the virtual G920 or wheelbri-uhid (the service applies profiles); false
    // for the in-game shim (the shim does). `mapping`: the session's own rotation in degrees ([steering]);
    // `overrides`: the user's per-game rotations ([game_rotation]). Logs the built-in profiles' problems, if any.
    GameWatch(std::string_view targetProfile, bool routePedals, float mapping, GameRotations overrides,
              GameFfbGains gains = {});

    struct Decision {
        PedalRoute route;           // Pipeline::setPedalRoute
        float ffbGain = 1.0F;       // FfbController::setGameGain: 1 = the game's forces as it asks
    };
    // One look: what the pipeline should use now.
    Decision check();

    // For the window's status, after "Running 1:23, ": the profile's label ("NFS 2015") while it is in use, else empty.
    [[nodiscard]] std::string status() const;
    // For the window, in red: what the player should fix ("NFS PAYBACK NEEDS THE VIRTUAL DEVICE", "NFS 2015: RESET ITS
    // WHEEL CONTROLS"), or empty.
    [[nodiscard]] const std::string& warning() const { return wayWarning_.empty() ? warning_ : wayWarning_; }
    // A game that does not work the way this session runs ([game] ways: NFS Payback with the shim): the advice while it
    // runs, and how many times such a game started this session (the app tells the user once per start).
    [[nodiscard]] const std::optional<WayAdvice>& wayAdvice() const { return wayNotices_.current(); }
    [[nodiscard]] std::uint64_t wayNotices() const { return wayNotices_.count(); }
    // For the window, after the label: the rotation the game steers with while it is not the mapping's own ("180
    // deg"), else empty: the wheel's own software must be set to it while the game runs.
    [[nodiscard]] std::string rotationStatus() const;

    // For the app's Game profiles tab: the game followed now, while its profile is in use.
    struct Live {
        std::string id;              // its profile's id; empty: none
        std::uint32_t pid = 0;
        std::uint64_t sinceMs = 0;   // platform::nowMs() when it started being followed
        std::uint64_t starts = 0;    // profiled games started (followed) this session: the tab follows each new one
        float ffbGain = 1.0F;        // the gain in use for it
    };
    [[nodiscard]] Live live() const;

private:
    void logEvents(const GameDetector::Update& update);
    void checkNfs2015Bindings();
    // The rotation for the game followed now (nullopt: the mapping's own), said in the log when it changes.
    [[nodiscard]] std::optional<float> currentRotation() const;
    void sayRotation();
    // The force feedback gain for the game followed now (1: none), said in the log when it changes.
    void sayFfbGain();
    [[nodiscard]] std::string describeFollowed() const;

    GameDetector detector_;
    std::string target_;
    bool routePedals_;
    float mapping_;
    GameRotations overrides_;
    std::optional<float> applied_;              // what the last check() said
    float ffbGain_ = 1.0F;                      // the same for the force feedback gain
    GameFfbGains ffbGains_;                     // the user's [game_ffb_gain]
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
