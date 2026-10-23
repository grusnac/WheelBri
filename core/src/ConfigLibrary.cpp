#include "wheelbri/ConfigLibrary.hpp"

#include "wheelbri/FileText.hpp"

#include "wheelbri/IniEditor.hpp"

#include <algorithm>
#include <cctype>

namespace wheelbri {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::optional<std::string> readText(const std::filesystem::path& path) { return readFileText(path); }

bool writeText(const std::filesystem::path& path, const std::string& text) { return writeFileReplacing(path, text); }

}  // namespace

std::string readAppSetting(const std::filesystem::path& stateFile, const std::string& key) {
    const std::optional<std::string> text = readText(stateFile);
    if (!text) return {};
    // Read raw, not through IniDocument: values are paths, and '#' or ';' in a folder name ("C:\Users\A#B") must not
    // start a comment there. A line is `key = value`; everything after the first '=' is the value, trimmed.
    const auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.remove_suffix(1);
        return v;
    };
    // The last match, as IniDocument reads a key written twice: writeAppSetting (IniEditor, which matches the exact
    // case) adds "config" after a hand-written "Config", and the value just written must be the one read back.
    std::string found;
    bool inApp = false;
    std::string_view rest(*text);
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        const std::string_view line = trim(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            inApp = lower(std::string(trim(line.substr(1, line.find(']') == std::string_view::npos
                                                                ? std::string_view::npos
                                                                : line.find(']') - 1)))) == "app";
            continue;
        }
        const std::size_t equals = line.find('=');
        if (!inApp || equals == std::string_view::npos) continue;
        if (lower(std::string(trim(line.substr(0, equals)))) == lower(key)) {
            found = std::string(trim(line.substr(equals + 1)));
        }
    }
    return found;
}

bool writeAppSetting(const std::filesystem::path& stateFile, const std::string& key, const std::string& value) {
    std::optional<std::string> text = readText(stateFile);
    std::error_code ec;
    if (!text && std::filesystem::exists(stateFile, ec)) {
        return false;   // there but unreadable for now (another program has it): writing would lose its other settings
    }
    IniEditor ini(text.value_or("; WheelBri app state: what was picked in the app. Safe to delete.\n"));
    // Replaced, never kept: IniEditor would keep "rig = Desk #2" when setting "Desk" (it reads '#' as a comment, so
    // the two look the same to it), and readAppSetting reads values raw.
    ini.remove("app", key);
    ini.set("app", key, value);
    return writeText(stateFile, ini.text());
}

std::string readConfigSelection(const std::filesystem::path& stateFile) { return readAppSetting(stateFile, "config"); }

bool writeConfigSelection(const std::filesystem::path& stateFile, const std::string& configPath) {
    return writeAppSetting(stateFile, "config", configPath);
}

}  // namespace wheelbri
