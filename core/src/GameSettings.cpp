#include "wheelbri/GameSettings.hpp"

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

}  // namespace wheelbri
