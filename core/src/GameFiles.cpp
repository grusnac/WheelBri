// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameFiles.hpp"

#include "wheelbri/GameSettings.hpp"

#include <optional>

namespace wheelbri {

namespace {

std::string joined(const std::vector<std::string>& problems) {
    std::string out;
    for (const std::string& problem : problems) out += (out.empty() ? "" : "; ") + problem;
    return out;
}

GameFilesResult change(const std::filesystem::path& folder, const FileValueMap& map, const bool add) {
    using Outcome = GameFilesResult::Outcome;
    GameFilesResult result;
    if (folder.empty()) {
        result.outcome = Outcome::NoFile;
        result.why = "the game's folder is not known";
        return result;
    }
    const std::optional<std::filesystem::path> file = gameFileUnder(folder, map.file, result.why);
    if (!file) {
        result.outcome = Outcome::Refused;
        return result;
    }
    result.file = *file;
    result.backup = result.file;
    result.backup += ".before-wheelbri";
    bool missing = false;
    bool retry = false;
    const std::optional<std::string> bytes = readGameFileBytes(result.file, result.why, missing, retry);
    if (!bytes) {
        result.outcome = missing ? Outcome::NoFile : retry ? Outcome::Failed : Outcome::Refused;
        if (missing) result.why = "no " + map.file + " in the game's folder";
        return result;
    }
    const FileValueEdit edit = add ? addFileValues(*bytes, map) : removeFileValues(*bytes, map);
    if (!edit.problems.empty()) {
        result.outcome = Outcome::Refused;
        result.why = joined(edit.problems);
        return result;
    }
    if (edit.changed == 0) {
        result.outcome = Outcome::AsAsked;
        return result;
    }
    if (const std::optional<std::string> why =
            writeGameFileKeepingOriginal(result.file, result.backup, *bytes, edit.text, retry)) {
        result.outcome = retry ? Outcome::Failed : Outcome::Refused;
        result.why = *why;
        return result;
    }
    result.outcome = Outcome::Written;
    result.changed = edit.changed;
    return result;
}

}  // namespace

GameFilesState readGameFilesState(const std::filesystem::path& folder, const FileValueMap& map) {
    using Status = GameFilesState::Status;
    GameFilesState state;
    state.lines.present.assign(map.lines.size(), false);
    if (folder.empty()) {
        state.why = "the game's folder is not known";
        return state;
    }
    const std::optional<std::filesystem::path> file = gameFileUnder(folder, map.file, state.why);
    if (!file) {
        state.status = Status::Unreadable;
        return state;
    }
    state.file = *file;
    bool missing = false;
    bool retry = false;
    const std::optional<std::string> bytes = readGameFileBytes(state.file, state.why, missing, retry);
    if (!bytes) {
        state.status = missing ? Status::NoFile : Status::Unreadable;
        if (missing) state.why = "no " + map.file + " in the game's folder";
        return state;
    }
    state.lines = fileValueState(*bytes, map);
    if (!state.lines.problems.empty()) {
        state.status = Status::Unreadable;
        state.why = joined(state.lines.problems);
        return state;
    }
    state.status = Status::Read;
    return state;
}

GameFilesResult addGameFiles(const std::filesystem::path& folder, const FileValueMap& map) {
    return change(folder, map, true);
}

GameFilesResult removeGameFiles(const std::filesystem::path& folder, const FileValueMap& map) {
    return change(folder, map, false);
}

}  // namespace wheelbri
