#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// The mapping configs the app picks from. Built-in ones come with the program (config/*.ini next to it, except the
// shim's wheelbri-shim.ini) and are the defaults: they are never changed. The user's own are made with New in a
// per-user folder, as a copy of a built-in one whose file name they keep in their first lines
// ("; made from: g920-virtual-device.ini"), so Reset can copy it again. The app remembers which one is picked in a small
// state file ([app] config = <path>).
struct ConfigEntry {
    std::string name;              // the file name without .ini
    std::filesystem::path path;
    bool builtIn = false;

    bool operator==(const ConfigEntry&) const = default;
};

// What New or Reset did: `path` is the config written, `error` why nothing was.
struct ConfigResult {
    bool ok = false;
    std::string error;
    std::filesystem::path path;
};

inline constexpr std::size_t kMaxConfigName = 40;

// The built-in configs (sorted by name), then the user's (sorted). A missing folder lists nothing.
[[nodiscard]] std::vector<ConfigEntry> listConfigs(const std::filesystem::path& builtInFolder,
                                                   const std::filesystem::path& userFolder);

// Why `name` cannot be a new config's name (empty when it can): 1 to kMaxConfigName letters, digits, spaces and
// - _ . ( ), not starting or ending with a space or a dot, and no config in `existing` called the same (ignoring
// case, as Windows does).
[[nodiscard]] std::string configNameProblem(const std::string& name, const std::vector<ConfigEntry>& existing);

// A new config's text: two comment lines (its name, the built-in file it was made from), then the built-in's text.
// Uses the built-in's line ends.
[[nodiscard]] std::string newConfigText(const std::string& name, const std::string& templateFile,
                                        std::string_view templateText);

// The built-in file a config was made from (its "; made from:" line, among the comments before the first
// section or key), or empty for a config not made with New.
[[nodiscard]] std::string configTemplateOf(std::string_view text);

// The built-in config Reset copies into `config`, if it has one that is in `configs`.
[[nodiscard]] std::optional<ConfigEntry> resetTemplateOf(const ConfigEntry& config,
                                                         const std::vector<ConfigEntry>& configs);

// New: writes <userFolder>/<name>.ini as a copy of the built-in `from` (making the folder).
ConfigResult createConfig(const std::filesystem::path& userFolder, const std::string& name, const ConfigEntry& from,
                          const std::vector<ConfigEntry>& existing);

// Save as: writes <userFolder>/<name>.ini with `text` (an edited copy of the built-in `from`), named as New names
// it, so Reset can put `from` back.
ConfigResult createConfigWithText(const std::filesystem::path& userFolder, const std::string& name,
                                  const ConfigEntry& from, std::string_view text,
                                  const std::vector<ConfigEntry>& existing);

// Reset: writes the built-in it was made from over `config` again. Fails for a built-in config (it is the default)
// and for one not made with New.
ConfigResult resetConfig(const ConfigEntry& config, const std::vector<ConfigEntry>& configs);

// Any [app] setting in the app's state file (the picked config, the active rig), or empty; and saving one.
[[nodiscard]] std::string readAppSetting(const std::filesystem::path& stateFile, const std::string& key);
bool writeAppSetting(const std::filesystem::path& stateFile, const std::string& key, const std::string& value);

// The picked config in the app's state file, or empty (no file, no entry).
[[nodiscard]] std::string readConfigSelection(const std::filesystem::path& stateFile);
// Saves the picked config there (making the folder, keeping the rest of the file). False if it cannot be written.
bool writeConfigSelection(const std::filesystem::path& stateFile, const std::string& configPath);

}  // namespace wheelbri
