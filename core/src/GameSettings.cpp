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

float gameFfbGain(const GameProfile& game, const GameFfbGains& gains) {
    if (const auto own = gains.find(game.id); own != gains.end()) return own->second;
    return game.ffbGain.value_or(1.0F);
}

bool hasSteeringSetting(const GameProfile& game) {
    for (const GameSetting& setting : game.settings) {
        if (!setting.value) return true;
    }
    return false;
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

}  // namespace

GameSettingsFileResult writeGameSettingsFile(const std::filesystem::path& root, const std::string_view relative,
                                             const std::span<const GameSettingText> values) {
    using Outcome = GameSettingsFileResult::Outcome;
    GameSettingsFileResult result;
    const std::filesystem::path rel = pathFromUtf8(std::string(relative));
    // / separated and relative, on every system: no drive ("C:x" is relative on Windows yet leaves `root`), no
    // backslash (a Windows separator, or a share), no root ("/x" has no drive on Windows but root / "/x" still leaves
    // `root`).
    if (root.empty() || rel.empty() || rel.has_root_path() || relative.find(':') != std::string_view::npos ||
        relative.find('\\') != std::string_view::npos) {
        result.outcome = Outcome::Refused;
        result.why = "no settings folder, or not a path under it";
        return result;
    }
    result.file = root / rel;
    result.backup = result.file;
    result.backup += ".before-wheelbri";

    // Every folder from `root` down, and the file: real ones, no links.
    std::filesystem::path walk = root;
    for (const std::filesystem::path& part : rel) {
        if (part == ".." || part == ".") {
            result.outcome = Outcome::Refused;
            result.why = "the path leaves the settings folder";
            return result;
        }
        walk /= part;
        if (isLink(walk)) {
            result.outcome = Outcome::Refused;
            result.why = pathToUtf8(walk) + " is a link: not followed";
            return result;
        }
    }
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(result.file, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        result.outcome = Outcome::NoFile;
        result.why = "no settings file yet";
        return result;
    }
    if (!std::filesystem::is_regular_file(status)) {
        result.outcome = Outcome::Refused;
        result.why = "not a plain file";
        return result;
    }
    const std::uintmax_t size = std::filesystem::file_size(result.file, ec);
    if (ec) {
        result.outcome = Outcome::Failed;
        result.why = "cannot read its size (" + ec.message() + ")";
        return result;
    }
    if (size > kMaxGameSettingsFileBytes) {
        result.outcome = Outcome::Refused;
        result.why = "larger than a settings file (" + std::to_string(size) + " bytes)";
        return result;
    }
    const std::optional<std::string> bytes = readFileText(result.file);
    if (!bytes) {
        result.outcome = Outcome::Failed;
        result.why = "cannot read it";
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

}  // namespace wheelbri
