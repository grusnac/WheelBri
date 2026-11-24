// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/FileValueMap.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

// A game profile's value map ([game_files], FileValueMap.hpp) on disk: the game's own file read for the Games tab, and
// every line of the map put in or taken out at once when the user presses its button there. The file is `folder` /
// map.file, `folder` the game's own folder (where its program is; empty: not known yet). Written as the game's
// settings are (GameSettings.hpp): checked first, all or nothing, through a temporary file and a rename, the file as
// it was kept once next to it as "<file>.before-wheelbri", no link followed (the service may run elevated).
namespace wheelbri {

struct GameFilesState {
    enum class Status {
        NoFolder,     // the game's folder is not known
        NoFile,       // no such file in it
        Unreadable,   // there, but not as expected or not readable now (`why`)
        Read,         // `lines` says which lines are in it
    };
    Status status = Status::NoFolder;
    std::string why;
    std::filesystem::path file;
    FileValueState lines;   // one per map line; all false unless Read
};
[[nodiscard]] GameFilesState readGameFilesState(const std::filesystem::path& folder, const FileValueMap& map);

struct GameFilesResult {
    enum class Outcome {
        Written,   // `changed` lines put in or taken out; the backup made if it was not there
        AsAsked,   // every line already in (add) or none in (remove): nothing written
        NoFile,    // no folder, or no such file in it: nothing written
        Refused,   // the file is not as expected (`why`): nothing written, and trying again will not help
        Failed,    // could not read or write it (`why`), e.g. the game holds it: worth trying again
    };
    Outcome outcome = Outcome::Failed;
    std::size_t changed = 0;
    std::string why;
    std::filesystem::path file;
    std::filesystem::path backup;
};
[[nodiscard]] GameFilesResult addGameFiles(const std::filesystem::path& folder, const FileValueMap& map);
[[nodiscard]] GameFilesResult removeGameFiles(const std::filesystem::path& folder, const FileValueMap& map);

}  // namespace wheelbri
