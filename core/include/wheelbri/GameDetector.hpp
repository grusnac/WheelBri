// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/GameProfiles.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// Decides which game with a profile (GameProfiles.hpp) the service follows, from what the system lists every few
// seconds. Portable and without a clock (the caller polls), so every branch is unit-tested:
//  - a game with a profile appears: followed at once (its pedals routed from the next state packet on);
//  - it disappears: still followed for `pollsToEnd` polls in a row (default 2: a launcher that restarts the game, or
//    a process list taken while it restarts, does not flip the pedals back and forth), then the next known game that
//    runs is followed, or none;
//  - it restarts within that time (another process id): still followed, said once (Restarted);
//  - two profiled games at once: the one already followed stays; when none was, the first in profile order. The
//    others are reported (Others) so the log can say which one the pedals do not follow;
//  - the system cannot list programs: nothing changes (the pedals never flip because of a failed listing), said once
//    (ListFailed) and again when listing works (ListBack);
//  - the profile was checked with another target (GameProfile::target): the game is followed and named, its profile
//    is not used (profileApplies false).
class GameDetector {
public:
    enum class Event : std::uint8_t {
        Started,     // a game is followed, none was
        Ended,       // the followed game is gone, none runs
        Switched,    // the followed game is gone, another known one runs: followed now
        Restarted,   // the followed game runs again under another process
        Others,      // the profiled games running but not followed changed
        ListFailed,  // the system could not list programs: everything kept
        ListBack,    // it can again
    };

    struct State {
        const GameProfile* game = nullptr;   // the game followed, null for none
        std::uint32_t pid = 0;
        std::string program;               // its name or path as listed
        bool profileApplies = false;       // its profile is for this target
        std::vector<RunningGame> others;   // profiled games running besides it, profile order
        bool listing = true;               // false while the system cannot list programs
    };

    struct Update {
        std::vector<Event> events;         // in the order they happened; empty: nothing changed
        const GameProfile* previous = nullptr;   // the game followed before this poll
    };

    // `target`: the target profile (target_profile, any case). `pollsToEnd`: misses before a game counts as ended
    // (at least 1). `profiles`: gameProfiles() (tests pass their own); kept by reference, it must outlive the detector.
    explicit GameDetector(std::string_view target, int pollsToEnd = 2,
                          std::span<const GameProfile> profiles = gameProfiles());

    // One look at the system: its programs, or nullopt when it could not list them.
    Update poll(const std::optional<std::vector<RunningProgram>>& programs);

    [[nodiscard]] const State& state() const { return state_; }

private:
    void follow(const RunningGame* running);

    std::string target_;
    int pollsToEnd_;
    std::span<const GameProfile> profiles_;
    int misses_ = 0;
    State state_;
};

}  // namespace wheelbri
