// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameSettings.hpp"

#include "wheelbri/FileText.hpp"

#include <cstdint>
#include <cstdio>

namespace wheelbri {

namespace {

constexpr std::string_view kFrostbiteMagic = "FBCHUNKS";
constexpr std::uint32_t kMaxValueBytes = 64;   // a number's text with its 0; anything longer is not a setting

std::uint32_t u32At(const std::string_view bytes, const std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (8 * i);
    }
    return value;
}

// Where `name`'s value text starts and how long it is (without its 0), for each well-formed entry of that name.
struct ValueSpan {
    std::size_t at;
    std::size_t length;
};

std::vector<ValueSpan> findValues(const std::string_view bytes, const std::string_view name) {
    std::vector<ValueSpan> found;
    std::string key(name);
    key.push_back('\0');
    for (std::size_t at = bytes.find(key); at != std::string_view::npos; at = bytes.find(key, at + 1)) {
        if (at < 8) continue;   // no room for the type and length before it
        if (u32At(bytes, at - 4) != key.size()) continue;
        const std::size_t lengthAt = at + key.size();
        if (lengthAt + 4 > bytes.size()) continue;
        const std::uint32_t valueBytes = u32At(bytes, lengthAt);
        if (valueBytes < 2 || valueBytes > kMaxValueBytes || lengthAt + 4 + valueBytes > bytes.size()) continue;
        const std::size_t valueAt = lengthAt + 4;
        if (bytes[valueAt + valueBytes - 1] != '\0') continue;
        bool printable = true;
        for (std::size_t i = 0; i + 1 < valueBytes; ++i) {
            const char c = bytes[valueAt + i];
            printable = printable && c >= 0x20 && c < 0x7F;
        }
        if (printable) found.push_back({valueAt, valueBytes - 1});
    }
    return found;
}

}  // namespace

std::optional<float> gameSettingValue(const GameProfile& game, const GameSetting& setting,
                                      const GameSettingOverrides& overrides) {
    if (!setting.value) return std::nullopt;
    if (const auto byGame = overrides.find(game.id); byGame != overrides.end()) {
        if (const auto own = byGame->second.find(setting.id); own != byGame->second.end()) return own->second;
    }
    return setting.value;
}

const GameSetting* findGameSetting(const GameProfile& game, const std::string_view id) {
    for (const GameSetting& setting : game.settings) {
        if (setting.id == id) return &setting;
    }
    return nullptr;
}

std::string gameSettingLabel(const std::string_view id) {
    std::string label(id);
    for (char& c : label) {
        if (c == '_') c = ' ';
    }
    return label;
}

std::string formatGameSettingText(const float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(value));
    return text;
}

std::vector<GameSettingText> gameSettingTexts(const GameProfile& game, const GameSettingOverrides& overrides,
                                              const float steeringDeg) {
    std::vector<GameSettingText> texts;
    for (const GameSetting& setting : game.settings) {
        const std::optional<float> value = gameSettingValue(game, setting, overrides);
        texts.push_back({setting.name, formatGameSettingText(value.value_or(steeringDeg))});
    }
    return texts;
}

GameSettingsEdit editFrostbiteSettings(const std::string_view bytes, const std::span<const GameSettingText> values) {
    GameSettingsEdit edit;
    if (bytes.substr(0, kFrostbiteMagic.size()) != kFrostbiteMagic) {
        edit.problems.push_back("not a Frostbite settings file (it does not start with FBCHUNKS)");
        return edit;
    }
    std::string out(bytes);
    for (const GameSettingText& value : values) {
        const std::vector<ValueSpan> found = findValues(bytes, value.name);
        if (found.size() != 1) {
            edit.problems.push_back(value.name +
                                    (found.empty() ? ": not in the file" : ": in the file more than once"));
            continue;
        }
        const std::string before(bytes.substr(found[0].at, found[0].length));
        if (value.text.size() != before.size()) {
            edit.problems.push_back(value.name + ": " + value.text + " is not as long as the file's " + before +
                                    " (only same-length values are written, so nothing else in the file moves)");
            continue;
        }
        if (before != value.text) {
            out.replace(found[0].at, before.size(), value.text);
            edit.changes.push_back({value.name, before, value.text});
        }
    }
    if (edit.problems.empty()) {
        edit.bytes = std::move(out);
    } else {
        edit.changes.clear();
    }
    return edit;
}

std::optional<std::string> frostbiteSetting(const std::string_view bytes, const std::string_view name) {
    if (bytes.substr(0, kFrostbiteMagic.size()) != kFrostbiteMagic) return std::nullopt;
    const std::vector<ValueSpan> found = findValues(bytes, name);
    if (found.size() != 1) return std::nullopt;
    return std::string(bytes.substr(found[0].at, found[0].length));
}

namespace {

// True when `path` itself is a link (or a reparse point the library reports as one); never follows it.
bool isLink(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
}

// The settings file at `root` / `relative`, checked as writeGameSettingsFile describes: nullopt and `why` when the
// path is not one under `root`, or a folder on the way is a link.
std::optional<std::filesystem::path> settingsFilePath(const std::filesystem::path& root,
                                                      const std::string_view relative, std::string& why) {
    const std::filesystem::path rel = pathFromUtf8(std::string(relative));
    // / separated and relative, on every system: no drive ("C:x" is relative on Windows yet leaves `root`), no
    // backslash (a Windows separator, or a share), no root ("/x" has no drive on Windows but root / "/x" still leaves
    // `root`).
    if (root.empty() || rel.empty() || rel.has_root_path() || relative.find(':') != std::string_view::npos ||
        relative.find('\\') != std::string_view::npos) {
        why = "no settings folder, or not a path under it";
        return std::nullopt;
    }
    // Every folder from `root` down, and the file: real ones, no links.
    std::filesystem::path walk = root;
    for (const std::filesystem::path& part : rel) {
        if (part == ".." || part == ".") {
            why = "the path leaves the settings folder";
            return std::nullopt;
        }
        walk /= part;
        if (isLink(walk)) {
            why = pathToUtf8(walk) + " is a link: not followed";
            return std::nullopt;
        }
    }
    return root / rel;
}

// The file's bytes, when it is a plain file of a settings file's size: nullopt and `why` otherwise (`missing` true
// when it is not there at all, `retry` true when reading it failed and may work later).
std::optional<std::string> readSettingsFile(const std::filesystem::path& file, std::string& why, bool& missing,
                                            bool& retry) {
    missing = false;
    retry = false;
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(file, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        missing = true;
        why = "no settings file yet";
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(status)) {
        why = "not a plain file";
        return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        retry = true;
        why = "cannot read its size (" + ec.message() + ")";
        return std::nullopt;
    }
    if (size > kMaxGameSettingsFileBytes) {
        why = "larger than a settings file (" + std::to_string(size) + " bytes)";
        return std::nullopt;
    }
    std::optional<std::string> bytes = readFileText(file);
    if (!bytes) {
        retry = true;
        why = "cannot read it";
    }
    return bytes;
}

}  // namespace

GameSettingsFileResult writeGameSettingsFile(const std::filesystem::path& root, const std::string_view relative,
                                             const std::span<const GameSettingText> values) {
    using Outcome = GameSettingsFileResult::Outcome;
    GameSettingsFileResult result;
    const std::optional<std::filesystem::path> file = settingsFilePath(root, relative, result.why);
    if (!file) {
        result.outcome = Outcome::Refused;
        return result;
    }
    result.file = *file;
    result.backup = result.file;
    result.backup += ".before-wheelbri";
    bool missing = false;
    bool retry = false;
    const std::optional<std::string> bytes = readSettingsFile(result.file, result.why, missing, retry);
    if (!bytes) {
        result.outcome = missing ? Outcome::NoFile : retry ? Outcome::Failed : Outcome::Refused;
        return result;
    }
    GameSettingsEdit edit = editFrostbiteSettings(*bytes, values);
    if (!edit.problems.empty()) {
        result.outcome = Outcome::Refused;
        for (const std::string& problem : edit.problems) result.why += (result.why.empty() ? "" : "; ") + problem;
        return result;
    }
    if (edit.changes.empty()) {
        result.outcome = Outcome::AsSet;
        return result;
    }
    // The backup: made once, from the bytes just read, never through a link and never over an existing one.
    std::error_code ec;
    const std::filesystem::file_status backupStatus = std::filesystem::symlink_status(result.backup, ec);
    if (backupStatus.type() == std::filesystem::file_type::not_found) {
        if (!writeFileReplacing(result.backup, *bytes)) {
            result.outcome = Outcome::Failed;
            result.why = "cannot keep a copy of the file as it was";
            return result;
        }
    } else if (!std::filesystem::is_regular_file(backupStatus)) {
        result.outcome = Outcome::Refused;
        result.why = pathToUtf8(result.backup) + " is there but is not a plain file";
        return result;
    }
    if (!writeFileReplacing(result.file, edit.bytes)) {
        result.outcome = Outcome::Failed;
        result.why = "cannot write it (the game may have it open)";
        return result;
    }
    result.outcome = Outcome::Written;
    result.changes = std::move(edit.changes);
    return result;
}

bool GameSettingsFileState::asSet() const {
    if (status != Status::Read) return false;
    for (const Entry& entry : entries) {
        if (entry.inFile != entry.wanted) return false;
    }
    return true;
}

GameSettingsFileState readGameSettingsState(const std::filesystem::path& root, const std::string_view relative,
                                            const std::span<const GameSettingText> values) {
    using Status = GameSettingsFileState::Status;
    GameSettingsFileState state;
    for (const GameSettingText& value : values) state.entries.push_back({value.name, value.text, std::nullopt});
    if (root.empty()) {
        state.status = Status::NotHere;
        state.why = "the game's own settings are only set on Windows for now";
        return state;
    }
    const std::optional<std::filesystem::path> file = settingsFilePath(root, relative, state.why);
    if (!file) {
        state.status = Status::Unreadable;
        return state;
    }
    state.file = *file;
    bool missing = false;
    bool retry = false;
    const std::optional<std::string> bytes = readSettingsFile(state.file, state.why, missing, retry);
    if (!bytes) {
        state.status = missing ? Status::NoFile : Status::Unreadable;
        return state;
    }
    if (bytes->substr(0, kFrostbiteMagic.size()) != kFrostbiteMagic) {
        state.status = Status::Unreadable;
        state.why = "not a Frostbite settings file (it does not start with FBCHUNKS)";
        return state;
    }
    state.status = Status::Read;
    for (GameSettingsFileState::Entry& entry : state.entries) entry.inFile = frostbiteSetting(*bytes, entry.name);
    return state;
}

bool settingsFileFits(const std::filesystem::path& file, const std::string_view settingsFile) {
    const auto lowerText = [](std::string text) {
        for (char& ch : text) ch = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
        return text;
    };
    std::vector<std::string> wanted;   // its parts from the file up: PROFILEOPTIONS, settings, the game's folder
    for (const auto& part : pathFromUtf8(std::string(settingsFile))) {
        if (!part.empty()) wanted.insert(wanted.begin(), lowerText(pathToUtf8(part)));
    }
    if (wanted.empty()) return false;
    std::filesystem::path at = file;
    for (const std::string& part : wanted) {
        if (at.empty() || !at.has_filename() || lowerText(pathToUtf8(at.filename())) != part) return false;
        at = at.parent_path();
    }
    return true;
}

std::vector<std::filesystem::path> settingsFilesWrittenBetween(const std::filesystem::path& documents,
                                                               const std::string_view settingsFile,
                                                               const std::filesystem::path& located,
                                                               const std::filesystem::file_time_type from,
                                                               const std::filesystem::file_time_type to) {
    std::vector<std::filesystem::path> found;
    const auto consider = [&found, from, to](const std::filesystem::path& file) {
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::symlink_status(file, ec);
        if (ec || !std::filesystem::is_regular_file(status)) return;   // a link is never followed
        const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, ec);
        if (ec || written < from || written > to) return;
        for (const std::filesystem::path& other : found) {
            if (sameSettingsFile(other, file)) return;
        }
        found.push_back(file);
    };
    // The settings file's path below its top folder ("settings/PROFILEOPTIONS"): in every folder of Documents.
    std::filesystem::path below;
    bool top = true;
    for (const auto& part : pathFromUtf8(std::string(settingsFile))) {
        if (part.empty()) continue;
        if (!top) below /= part;
        top = false;
    }
    if (!documents.empty() && !below.empty()) {
        std::error_code ec;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::directory_iterator it(documents, options, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code statusError;
            if (it->is_directory(statusError) && !it->is_symlink(statusError)) consider(it->path() / below);
        }
    }
    if (!located.empty()) consider(located);
    return found;
}

bool sameSettingsFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
        const bool same = std::filesystem::equivalent(a, b, ec);
        if (!ec) return same;
    }
    const auto lowerText = [](std::string text) {
        for (char& ch : text) ch = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
        return text;
    };
    return lowerText(pathToUtf8(a.lexically_normal().make_preferred())) ==
           lowerText(pathToUtf8(b.lexically_normal().make_preferred()));
}

}  // namespace wheelbri
