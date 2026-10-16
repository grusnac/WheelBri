#include "wheelbri/ConfigLibrary.hpp"

#include "wheelbri/FileText.hpp"

#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace wheelbri {

namespace {

constexpr std::string_view kMadeFrom = "; made from: ";

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string pathText(const std::filesystem::path& path) { return pathToUtf8(path); }

std::optional<std::string> readText(const std::filesystem::path& path) { return readFileText(path); }

bool writeText(const std::filesystem::path& path, const std::string& text) { return writeFileReplacing(path, text); }

std::vector<ConfigEntry> listFolder(const std::filesystem::path& folder, const bool builtIn) {
    std::vector<ConfigEntry> found;
    forEachEntry(folder, [&](const std::filesystem::directory_entry& entry) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || lower(pathText(entry.path().extension())) != ".ini") return;
        std::string name = pathText(entry.path().stem());
        if (name.empty()) return;   // a name that cannot be shown
        if (builtIn && lower(name).rfind("wheelbri-shim", 0) == 0) return;   // the shim's settings, not a mapping
        found.push_back({std::move(name), entry.path(), builtIn});
    });
    std::sort(found.begin(), found.end(),
              [](const ConfigEntry& a, const ConfigEntry& b) { return lower(a.name) < lower(b.name); });
    return found;
}

bool allowedInName(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' || c == '_' || c == '.' ||
           c == '(' || c == ')';
}

}  // namespace

std::vector<ConfigEntry> listConfigs(const std::filesystem::path& builtInFolder,
                                     const std::filesystem::path& userFolder) {
    std::vector<ConfigEntry> configs = listFolder(builtInFolder, true);
    std::vector<ConfigEntry> own = listFolder(userFolder, false);
    configs.insert(configs.end(), own.begin(), own.end());
    return configs;
}

std::string configNameProblem(const std::string& name, const std::vector<ConfigEntry>& existing) {
    if (name.empty()) return "type a name for the new config";
    if (name.size() > kMaxConfigName) return "at most " + std::to_string(kMaxConfigName) + " characters";
    if (!std::all_of(name.begin(), name.end(), allowedInName)) {
        return "use letters, digits, spaces and - _ . ( ) only";
    }
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.') {
        return "no space or dot at the start or the end";
    }
    // Windows' device names, also before an extension: "con.ini" is the console, not a file.
    const std::string base = lower(name.substr(0, name.find('.')));
    static constexpr std::array<const char*, 4> kDevices = {"con", "prn", "aux", "nul"};
    const bool numbered = base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) &&
                          base[3] >= '0' && base[3] <= '9';
    if (numbered || std::find(kDevices.begin(), kDevices.end(), base) != kDevices.end()) {
        return "'" + name + "' is a name Windows keeps for a device: choose another";
    }
    for (const ConfigEntry& config : existing) {
        if (lower(config.name) == lower(name)) return "there is already a config called '" + config.name + "'";
    }
    return {};
}

std::string newConfigText(const std::string& name, const std::string& templateFile, std::string_view templateText) {
    const std::string newline = templateText.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    std::string text = "; WheelBri mapping config '" + name + "', made with New in the app. Reset in the app copies" +
                       newline + "; the built-in config below over it again, dropping every change." + newline;
    text += std::string(kMadeFrom) + templateFile + newline + newline;
    // A UTF-8 byte order mark would sit in the middle of the file now.
    if (templateText.substr(0, 3) == "\xEF\xBB\xBF") templateText.remove_prefix(3);
    text += templateText;
    return text;
}

std::string configTemplateOf(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end + 1;
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos) continue;
        line.remove_prefix(first);
        if (line.front() != ';' && line.front() != '#') break;   // the comments at the top only
        if (line.substr(0, kMadeFrom.size()) == kMadeFrom) {
            std::string file(line.substr(kMadeFrom.size()));
            while (!file.empty() && (file.back() == ' ' || file.back() == '\t')) file.pop_back();
            return file;
        }
    }
    return {};
}

std::optional<ConfigEntry> resetTemplateOf(const ConfigEntry& config, const std::vector<ConfigEntry>& configs) {
    if (config.builtIn) return std::nullopt;
    const std::optional<std::string> text = readText(config.path);
    if (!text) return std::nullopt;
    std::string made = lower(configTemplateOf(*text));
    if (made.empty()) return std::nullopt;
    // Copies made before the built-in configs were renamed name the old files: for the rig (2026-10-04; the G29 is
    // gone), then one per mode (until 2026-10-15). One per target now: g920.ini, g25.ini, g27.ini, g923.ini.
    if (made == "moza-r3-as-g920.ini" || made == "moza-r3-as-g29.ini" || made == "rig-as-g920.ini" ||
        made == "rig-as-g29.ini") {
        made = "g920.ini";
    }
    for (const char* suffix : {"-virtual-device.ini", "-in-game-shim.ini"}) {
        const std::string tail(suffix);
        if (made.size() > tail.size() && made.compare(made.size() - tail.size(), tail.size(), tail) == 0) {
            made = made.substr(0, made.size() - tail.size()) + ".ini";
        }
    }
    for (const ConfigEntry& candidate : configs) {
        if (candidate.builtIn && lower(pathText(candidate.path.filename())) == made) return candidate;
    }
    return std::nullopt;
}

ConfigResult createConfig(const std::filesystem::path& userFolder, const std::string& name, const ConfigEntry& from,
                          const std::vector<ConfigEntry>& existing) {
    ConfigResult result;
    if (const std::string problem = configNameProblem(name, existing); !problem.empty()) {
        result.error = problem;
        return result;
    }
    const std::optional<std::string> templateText = readText(from.path);
    if (!templateText) {
        result.error = "cannot read " + pathText(from.path);
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(userFolder, ec);
    result.path = userFolder / (name + ".ini");   // ASCII only (configNameProblem)
    if (std::filesystem::exists(result.path, ec)) {
        result.error = pathText(result.path) + " already exists";
        return result;
    }
    if (!writeText(result.path, newConfigText(name, pathText(from.path.filename()), *templateText))) {
        result.error = "cannot write " + pathText(result.path) + (ec ? ": " + ec.message() : std::string());
        return result;
    }
    result.ok = true;
    return result;
}

ConfigResult createConfigWithText(const std::filesystem::path& userFolder, const std::string& name,
                                  const ConfigEntry& from, const std::string_view text,
                                  const std::vector<ConfigEntry>& existing) {
    ConfigResult result;
    if (const std::string problem = configNameProblem(name, existing); !problem.empty()) {
        result.error = problem;
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(userFolder, ec);
    result.path = userFolder / (name + ".ini");   // ASCII only (configNameProblem)
    if (std::filesystem::exists(result.path, ec)) {
        result.error = pathText(result.path) + " already exists";
        return result;
    }
    if (!writeText(result.path, newConfigText(name, pathText(from.path.filename()), text))) {
        result.error = "cannot write " + pathText(result.path);
        return result;
    }
    result.ok = true;
    return result;
}

ConfigResult resetConfig(const ConfigEntry& config, const std::vector<ConfigEntry>& configs) {
    ConfigResult result;
    result.path = config.path;
    if (config.builtIn) {
        result.error = "'" + config.name + "' is built in: it is the default already";
        return result;
    }
    const std::optional<std::string> text = readText(config.path);
    if (!text) {
        result.error = "cannot read " + pathText(config.path);
        return result;
    }
    const std::string made = configTemplateOf(*text);
    if (made.empty()) {
        result.error = "'" + config.name + "' was not made with New, so there is nothing to reset it to";
        return result;
    }
    const std::optional<ConfigEntry> from = resetTemplateOf(config, configs);
    if (!from) {
        result.error = "'" + config.name + "' was made from " + made + ", which is not installed";
        return result;
    }
    const std::optional<std::string> templateText = readText(from->path);
    if (!templateText) {
        result.error = "cannot read " + pathText(from->path);
        return result;
    }
    if (!writeText(config.path, newConfigText(config.name, pathText(from->path.filename()), *templateText))) {
        result.error = "cannot write " + pathText(config.path);
        return result;
    }
    result.ok = true;
    return result;
}

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
            return std::string(trim(line.substr(equals + 1)));
        }
    }
    return {};
}

bool writeAppSetting(const std::filesystem::path& stateFile, const std::string& key, const std::string& value) {
    std::optional<std::string> text = readText(stateFile);
    std::error_code ec;
    if (!text && std::filesystem::exists(stateFile, ec)) {
        return false;   // there but unreadable for now (another program has it): writing would lose its other settings
    }
    IniEditor ini(text.value_or("; WheelBri app state: what was picked in the app. Safe to delete.\n"));
    ini.set("app", key, value);
    return writeText(stateFile, ini.text());
}

std::string readConfigSelection(const std::filesystem::path& stateFile) { return readAppSetting(stateFile, "config"); }

bool writeConfigSelection(const std::filesystem::path& stateFile, const std::string& configPath) {
    return writeAppSetting(stateFile, "config", configPath);
}

}  // namespace wheelbri
