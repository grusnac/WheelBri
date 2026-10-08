#pragma once

#include "wheelbri/Logging.hpp"

#include <string>

namespace wheelbri::shim {

// Sets up logging for the proxy DLL on first use: %LOCALAPPDATA%\wheelbri\logs\wheelbri-shim-<date>-<NN>.log (one
// per run of the game), no console, every line flushed at once (there is no background flush thread inside the
// game process).
// Level defaults to debug; override per component with SPDLOG_LEVEL, e.g. "debug,shim.state=trace".
// Never call from DllMain: it creates files and takes locks.
void ensureLoggingInitialised();

// Called from DLL_PROCESS_DETACH. Only flushes when the DLL is unloaded with FreeLibrary; at process
// exit other threads are already gone and may have died holding logging locks, so nothing is touched.
// Does nothing if logging was never initialised, and never throws.
void onDllDetach(bool processTerminating) noexcept;

[[nodiscard]] logging::Logger logger(const char* component);

// Folder that holds this DLL, with a trailing separator (config and log files live next to it).
[[nodiscard]] std::wstring dllDirectory();

}  // namespace wheelbri::shim
