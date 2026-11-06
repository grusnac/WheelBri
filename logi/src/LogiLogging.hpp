// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Logging.hpp"

namespace wheelbri::logi {

// Logging for wheelbri-logi.dll, set up on first use: %LOCALAPPDATA%\wheelbri\logs\wheelbri-logi-<date>-<NN>.log (one
// per run of the game), no console, every line flushed at once (no background thread inside the game). The level is
// [logi] log_level in wheelbri-logi.ini next to the DLL (scripts/make/logi.sh writes debug there), else the app's (its
// Logs tab's Level button, read from app.ini at start; warn without one); SPDLOG_LEVEL still sets it per component
// at start, e.g. "info,logi.input=debug". Never call from DllMain.
void ensureLoggingInitialised();

// From DLL_PROCESS_DETACH: flushes only on FreeLibrary (at process exit other threads may hold logging locks), never
// throws, does nothing before the first export call.
void onDllDetach(bool processTerminating) noexcept;

[[nodiscard]] logging::Logger logger(const char* component);

// An exception stopped at the boundary to the game (logged, rate-limited; never throws).
void noteEscapedException(const char* where, const char* what) noexcept;

}  // namespace wheelbri::logi
