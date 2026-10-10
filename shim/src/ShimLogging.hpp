#pragma once

#include "LogFolder.hpp"

#include "wheelbri/LogLevel.hpp"
#include "wheelbri/Logging.hpp"

#include <string>

namespace wheelbri::shim {

// Sets up logging for the proxy DLL on first use: %LOCALAPPDATA%\wheelbri\logs\wheelbri-shim-<date>-<NN>.log (one
// per run of the game), no console, every line flushed at once (there is no background flush thread inside the
// game process).
// Level: the app's (its Logs tab's Level button), read from %LOCALAPPDATA%\wheelbri\app.ini at start (warn when there
// is none, e.g. under Proton) and then from the service's LogLevelPacket (StateFeed, applyLogLevel). SPDLOG_LEVEL
// still sets it per component at start, e.g. "debug,shim.state=trace".
// Never call from DllMain: it creates files and takes locks.
void ensureLoggingInitialised();

// Called from DLL_PROCESS_DETACH. Only flushes when the DLL is unloaded with FreeLibrary; at process
// exit other threads are already gone and may have died holding logging locks, so nothing is touched.
// Does nothing if logging was never initialised, and never throws.
void onDllDetach(bool processTerminating) noexcept;

[[nodiscard]] logging::Logger logger(const char* component);

// The service said another log level (LogLevelPacket): applied once per change, with a line saying so. Cheap when it
// is the same (the game's input thread calls it).
void applyLogLevel(LogLevel level);

// Folder that holds this DLL, with a trailing separator (config files live next to it).
using gamelog::dllDirectory;

}  // namespace wheelbri::shim
