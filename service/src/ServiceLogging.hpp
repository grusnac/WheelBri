// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Logging.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace wheelbri::service {

// Per-user log location: %LOCALAPPDATA%\wheelbri\logs, ~/.local/state/wheelbri (XDG) or ~/Library/Logs/wheelbri.
[[nodiscard]] std::string defaultLogFile();

// Where the older Wheel setup tab kept what each device is and what its controls are called (DeviceLabels.hpp, one
// <vid>-<pid>.ini per device), still read where no rig names a device: %LOCALAPPDATA%\wheelbri\devices,
// ~/.config/wheelbri/devices (XDG) or ~/Library/Application Support/wheelbri/devices. Nothing writes there now.
[[nodiscard]] std::filesystem::path deviceLabelsFolder();

// Where the Mapping tab saves the user's own mapping (my-<target>.ini, ConfigPicker.hpp):
// %LOCALAPPDATA%\wheelbri\configs, ~/.config/wheelbri/configs (XDG) or ~/Library/Application Support/wheelbri/configs.
[[nodiscard]] std::filesystem::path userConfigFolder();

// Where the Wheel setup tab keeps the rigs (Rig.hpp, <name>.ini): ...\wheelbri\rigs.
[[nodiscard]] std::filesystem::path rigsFolder();

// The app's state file (which config is picked): app.ini in the folder above those two.
[[nodiscard]] std::filesystem::path appStateFile();
// The games the user added in the Games tab (GameTuning.hpp): games.ini next to app.ini.
[[nodiscard]] std::filesystem::path userGamesFile();

// The games the in-game shim was put into (the app, target G920 in-game shim; ShimGames.hpp): shim-games.txt next
// to app.ini, per user like every other setting.
[[nodiscard]] std::filesystem::path shimGamesFile();

// %LOCALAPPDATA%\wheelbri, ~/.config/wheelbri (XDG) or ~/Library/Application Support/wheelbri.
[[nodiscard]] std::filesystem::path userSettingsFolder();

// Whether `path` is inside the user's WheelBri folders (settings or logs). Log and telemetry paths from a config must
// be: the app runs as Administrator and a config can be written by any program of the user.
[[nodiscard]] bool isInsideUserFolders(const std::filesystem::path& path);

// Windows: a reason the user's WheelBri folders must not be written to (one of them is a link to elsewhere), or "".
[[nodiscard]] std::string userFoldersProblem();

// Windows: deletes links among the log and telemetry files in `folder` (a symbolic link, or a file with a second hard
// link), which the app, running as Administrator, would otherwise append to wherever they point. Only the link goes,
// never what it points to. Returns what it did, for the log. Does nothing on other systems.
[[nodiscard]] std::vector<std::string> removeLinkedLogFiles(const std::filesystem::path& folder);

// Tells the user why WheelBri cannot start: a message box in the window build (`window`), stderr otherwise.
void reportStartupProblem(const std::string& text, bool window);

// One folder for every log, the user's (Windows: %LOCALAPPDATA%\wheelbri\logs): the drivers run as LocalService and
// would otherwise write to C:\ProgramData\wheelbri\logs. As Administrator (the app) this lets LocalService write in
// `folder`, tells the drivers where it is (HKLM\SOFTWARE\wheelbri LogFolder, read for every line they write) and
// moves what is still in C:\ProgramData\wheelbri\logs into it. Not Administrator, or not Windows: does nothing (on
// Linux every log is in one folder already). Logs what it did; call it once logging runs.
void shareLogFolder(const std::filesystem::path& folder);

// Writes the version and platform at startup so every log is self-describing.
void logStartupBanner(const std::string& configPath);

}  // namespace wheelbri::service
