#pragma once

#include <filesystem>
#include <string>

namespace wheelbri {

// The app's state file: what was picked in the app ([app] config = <path>, the target, the mode, the active rig).
// Any [app] setting in it, or empty; and saving one. Values are read raw (paths may hold '#' or ';').
[[nodiscard]] std::string readAppSetting(const std::filesystem::path& stateFile, const std::string& key);
bool writeAppSetting(const std::filesystem::path& stateFile, const std::string& key, const std::string& value);

// The picked config in the app's state file, or empty (no file, no entry).
[[nodiscard]] std::string readConfigSelection(const std::filesystem::path& stateFile);
// Saves the picked config there (making the folder, keeping the rest of the file). False if it cannot be written.
bool writeConfigSelection(const std::filesystem::path& stateFile, const std::string& configPath);

}  // namespace wheelbri
