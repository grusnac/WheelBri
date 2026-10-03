#include "wheelbri/ConfigLibrary.hpp"

#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"

#include <algorithm>
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

std::string pathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    return static_cast<bool>(file);
}

std::vector<ConfigEntry> listFolder(const std::filesystem::path& folder, const bool builtIn) {
    std::vector<ConfigEntry> found;
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return found;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || lower(pathText(entry.path().extension())) != ".ini") continue;
        std::string name = pathText(entry.path().stem());
        if (builtIn && lower(name).rfind("wheelbri-shim", 0) == 0) continue;   // the shim's settings, not a mapping
        found.push_back({std::move(name), entry.path(), builtIn});
    }
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
    // Copies made before the built-in configs were renamed for the rig (2026-10-04) name the old files.
    if (made == "moza-r3-as-g920.ini") made = "rig-as-g920.ini";
    if (made == "moza-r3-as-g29.ini") made = "rig-as-g29.ini";
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
    return IniDocument::parse(*text).document.get("app", key).value_or("");
}

bool writeAppSetting(const std::filesystem::path& stateFile, const std::string& key, const std::string& value) {
    std::error_code ec;
    std::filesystem::create_directories(stateFile.parent_path(), ec);
    IniEditor ini(readText(stateFile).value_or("; WheelBri app state: what was picked in the app. Safe to delete.\n"));
    ini.set("app", key, value);
    return writeText(stateFile, ini.text());
}

std::string readConfigSelection(const std::filesystem::path& stateFile) { return readAppSetting(stateFile, "config"); }

bool writeConfigSelection(const std::filesystem::path& stateFile, const std::string& configPath) {
    return writeAppSetting(stateFile, "config", configPath);
}

}  // namespace wheelbri
