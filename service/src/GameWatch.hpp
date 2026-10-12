#pragma once

#include "wheelbri/GameDetector.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"

#include <cstdint>
#include <filesystem>
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
// [game_rotation] line; GameRotation.hpp) steers with it while it runs: check() returns the ratio for the pipeline,
// the log says what changed and that the wheel's own software must be set to the same, and the window shows it.
// A game with a force feedback gain (its profile's [ffb] gain: NFS Unbound) gets it while it runs: check() returns it
// for the service's FfbController, and the log says so. A game with its own settings to set (its profile's
// [game_settings], GameSettings.hpp: NFS Unbound's hidden wheel settings) gets them written into its settings file
// while it is NOT running: at the first check() and again whenever it ends (it reads the file when it starts and
// writes its own values back when it ends); while it runs the log says they follow when it closes.
// Called from the session's window thread every 2 s (a few milliseconds; never on the service loop); not thread-safe.
class GameWatch {
public:
    // `routePedals`: the session publishes to the virtual G920 or wheelbri-uhid (the service applies profiles); false
    // for the in-game shim (the shim does). `mapping`: the session's own rotation ([steering]); `overrides`: the
    // user's per-game rotations ([game_rotation]). Logs the built-in profiles' problems, if any.
    GameWatch(std::string_view targetProfile, bool routePedals, SteeringRotation mapping, GameRotations overrides,
              GameSettingOverrides settings = {}, GameFfbGains gains = {});

    struct Decision {
        PedalRoute route;           // Pipeline::setPedalRoute
        float steeringRatio = 0;    // Pipeline::setSteeringRatio: 0 = the mapping's own
        float ffbGain = 1.0F;       // FfbController::setGameGain: 1 = the game's forces as it asks
    };
    // One look: what the pipeline should use now.
    Decision check();

    // For the window's status, after "Running 1:23, ": the profile's label ("NFS 2015") while it is in use, else empty.
    [[nodiscard]] std::string status() const;
    // For the window, in red: what the player should fix ("NFS 2015: RESET ITS WHEEL CONTROLS"), or empty.
    [[nodiscard]] const std::string& warning() const { return warning_; }
    // For the window, after the label: the rotation the game steers with while it is not the mapping's own ("180
    // deg"), else empty. The log says the wheel's own software must be set to the same.
    [[nodiscard]] std::string rotationStatus() const;

private:
    void logEvents(const GameDetector::Update& update);
    void checkNfs2015Bindings();
    // The rotation for the game followed now (nullopt: the mapping's own), said in the log when it changes.
    [[nodiscard]] std::optional<SteeringRotation> currentRotation() const;
    void sayRotation();
    // The force feedback gain for the game followed now (1: none), said in the log when it changes.
    void sayFfbGain();
    // Games' own settings: queued at the first check() and when a game with settings ends; written once it is not
    // running (and the running programs could be listed).
    void queueGameSettings(const GameDetector::Update& update);
    void writePendingGameSettings();
    void writeGameSettings(const GameProfile& game);
    [[nodiscard]] bool running(const GameProfile& game) const;
    [[nodiscard]] std::string describeFollowed() const;

    GameDetector detector_;
    std::string target_;
    bool routePedals_;
    SteeringRotation mapping_;
    GameRotations overrides_;
    std::optional<SteeringRotation> applied_;   // what the last check() applied
    float ffbGain_ = 1.0F;                      // the same for the force feedback gain
    GameSettingOverrides settingOverrides_;     // the user's [game_settings]
    GameFfbGains ffbGains_;                     // the user's [game_ffb_gain]
    bool settingsQueued_ = false;               // the first check() queued every game with settings
    std::set<std::string> settingsPending_;     // profile ids whose settings are still to be written
    std::set<std::string> settingsWaitSaid_;    // "set after it closes" said for these, while they run
    bool noDocumentsSaid_ = false;
    const GameProfile* unroutable_ = nullptr;   // a profile the device cannot apply, said once
    std::string warning_;
    // NFS 2015's settings file: checked again only when its last write changes.
    std::optional<std::filesystem::file_time_type> settingsWritten_;
};

}  // namespace wheelbri::service
