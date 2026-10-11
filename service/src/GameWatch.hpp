#pragma once

#include "wheelbri/GameDetector.hpp"
#include "wheelbri/GameProfiles.hpp"

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
// undo the profile. A profile's steering advice ([steering] rotation_deg and note) is said in the log and the window
// when its game runs and the session's game rotation is another: advice only, nothing is changed. Called from the session's window thread every 2 s (a few
// milliseconds; never on the service loop); not thread-safe.
class GameWatch {
public:
    // `routePedals`: the session publishes to the virtual G920 or wheelbri-uhid (the service applies profiles); false
    // for the in-game shim (the shim does). `gameRotationDeg`: the session's [steering] target_rotation_deg (the
    // profiles' advice is compared with it). Logs the built-in profiles' problems, if any.
    GameWatch(std::string_view targetProfile, bool routePedals, float gameRotationDeg);

    // One look: returns the pedal route to publish (Pipeline::setPedalRoute).
    PedalRoute check();

    // For the window's status, after "Running 1:23, ": the profile's label ("NFS 2015") while it is in use, else empty.
    [[nodiscard]] std::string status() const;
    // For the window, in red: what the player should fix ("NFS 2015: RESET ITS WHEEL CONTROLS"), or empty.
    [[nodiscard]] const std::string& warning() const { return warning_; }
    // For the window, in amber after the label: the profile's steering advice while its game runs and the session's
    // rotation is another ("180 deg advised"), else empty. The log has the reason.
    [[nodiscard]] std::string advice() const;

private:
    void logEvents(const GameDetector::Update& update);
    void checkNfs2015Bindings();
    void sayAdvice() const;
    [[nodiscard]] std::string describeFollowed() const;

    GameDetector detector_;
    std::string target_;
    bool routePedals_;
    float gameRotationDeg_;
    const GameProfile* unroutable_ = nullptr;   // a profile the device cannot apply, said once
    std::string warning_;
    // NFS 2015's settings file: checked again only when its last write changes.
    std::optional<std::filesystem::file_time_type> settingsWritten_;
};

}  // namespace wheelbri::service
