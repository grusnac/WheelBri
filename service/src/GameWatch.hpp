#pragma once

#include "wheelbri/GameDetector.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameRotation.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
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
// Called from the session's window thread every 2 s (a few milliseconds; never on the service loop); not thread-safe.
class GameWatch {
public:
    // `routePedals`: the session publishes to the virtual G920 or wheelbri-uhid (the service applies profiles); false
    // for the in-game shim (the shim does). `mapping`: the session's own rotation ([steering]); `overrides`: the
    // user's per-game rotations ([game_rotation]). Logs the built-in profiles' problems, if any.
    GameWatch(std::string_view targetProfile, bool routePedals, SteeringRotation mapping, GameRotations overrides);

    struct Decision {
        PedalRoute route;           // Pipeline::setPedalRoute
        float steeringRatio = 0;    // Pipeline::setSteeringRatio: 0 = the mapping's own
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
    [[nodiscard]] std::string describeFollowed() const;

    GameDetector detector_;
    std::string target_;
    bool routePedals_;
    SteeringRotation mapping_;
    GameRotations overrides_;
    std::optional<SteeringRotation> applied_;   // what the last check() applied
    const GameProfile* unroutable_ = nullptr;   // a profile the device cannot apply, said once
    std::string warning_;
    // NFS 2015's settings file: checked again only when its last write changes.
    std::optional<std::filesystem::file_time_type> settingsWritten_;
};

}  // namespace wheelbri::service
