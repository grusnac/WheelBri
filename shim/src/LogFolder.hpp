#pragma once

#include "wheelbri/LogLevel.hpp"

#include <windows.h>

#include <string>

// Where a DLL that runs inside a game (the dinput8.dll shim, wheelbri-logi.dll) writes its log, and at which level it
// starts. Shared by both DLLs: each compiles this file, so "this module" is the DLL it is part of.
namespace wheelbri::gamelog {

// The full path of `module` (nullptr: the game's .exe).
[[nodiscard]] std::wstring modulePath(HMODULE module);
// The DLL this code is linked into.
[[nodiscard]] HMODULE thisModule();
// Folder that holds this DLL, with a trailing separator (config files live next to it).
[[nodiscard]] std::wstring dllDirectory();

// UTF-8 of a wide string (paths for the log).
[[nodiscard]] std::string utf8(const std::wstring& text);

// The level the app picked last ([app] log_level in %LOCALAPPDATA%\wheelbri\app.ini; under Wine the Linux app's
// ~/.config/wheelbri/app.ini), warn without one.
[[nodiscard]] LogLevel appLevelAtStart();

// The log file for `fileName` ("wheelbri-shim.log"; logging::init adds the date and the run's number), next to the
// service's logs and telemetry (%LOCALAPPDATA%\wheelbri\logs), so every file is in one folder that the app's Logs tab
// shows and its [retention] settings clean up. Falls back to the DLL's folder if LOCALAPPDATA is not set. Under Wine:
// the Linux service's log folder (~/.local/state/wheelbri). A folder with non-ASCII letters is given by its 8.3 short
// name (see LogFolder.cpp).
[[nodiscard]] std::string logFilePath(const std::wstring& fileName);

// True when running under Wine (Proton); `version` gets Wine's version then.
[[nodiscard]] bool underWine(std::string& version);

}  // namespace wheelbri::gamelog
