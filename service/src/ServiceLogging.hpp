#pragma once

#include "wheelbri/Logging.hpp"

#include <filesystem>
#include <string>

namespace wheelbri::service {

// Per-user log location: %LOCALAPPDATA%\wheelbri\logs, ~/.local/state/wheelbri (XDG) or ~/Library/Logs/wheelbri.
[[nodiscard]] std::string defaultLogFile();

// Where the Wheel setup tab saves what each device is and what its controls are called (DeviceLabels.hpp, one
// <vid>-<pid>.ini per device): %LOCALAPPDATA%\wheelbri\devices, ~/.config/wheelbri/devices (XDG) or
// ~/Library/Application Support/wheelbri/devices. Per user, so saving needs no Administrator.
[[nodiscard]] std::filesystem::path deviceLabelsFolder();

// Where the app's New puts the user's own mapping configs (ConfigLibrary.hpp): %LOCALAPPDATA%\wheelbri\configs,
// ~/.config/wheelbri/configs (XDG) or ~/Library/Application Support/wheelbri/configs.
[[nodiscard]] std::filesystem::path userConfigFolder();

// Where the Wheel setup tab keeps the rigs (Rig.hpp, <name>.ini): ...\wheelbri\rigs.
[[nodiscard]] std::filesystem::path rigsFolder();

// The app's state file (which config is picked): app.ini in the folder above those two.
[[nodiscard]] std::filesystem::path appStateFile();

// The project was called wheelemu before 2026-10-03: moves the user's folders from that name to wheelbri (rigs,
// configs, app.ini, logs, telemetry), once, before anything opens them. Entries already in the new folder are kept
// and the old one is left with what could not move. Paths to the old folder inside app.ini are rewritten. Call it
// first in main(); logStartupBanner logs what it did.
void moveFoldersFromOldName();

// Writes the version and platform at startup so every log is self-describing.
void logStartupBanner(const std::string& configPath);

}  // namespace wheelbri::service
