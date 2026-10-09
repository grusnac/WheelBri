#pragma once

#include "wheelbri/PedalLayout.hpp"

#include <span>
#include <string>
#include <string_view>

namespace wheelbri {

// Games WheelBri knows by their program, and how each reads the emulated G920's pedals. The service looks at the
// running programs every few seconds and gives the first known game it finds its pedal layout; with none running
// (or one not listed) the pedals are where a real G920 has them. A detection, not a setting: nothing to choose.
struct KnownGame {
    std::string_view name;      // "NFS 2015": for the log and the window
    std::string_view program;   // the program's file name, lower case: "nfs16.exe"
    PedalLayout pedals;
};

// Every known game, in the order they are looked for.
[[nodiscard]] std::span<const KnownGame> knownGames();

// The program's file name, lower case, from what the system says about a process: a bare name ("NFS16.exe") or a
// path with / or \ separators (a Wine process's command line on Linux: "Z:\games\NFS16.exe"). Empty for empty input.
[[nodiscard]] std::string programName(std::string_view nameOrPath);

// The first known game (table order) among `programs` (names or paths, any case); null when none of them runs.
[[nodiscard]] const KnownGame* findRunningGame(std::span<const std::string> programs);

}  // namespace wheelbri
