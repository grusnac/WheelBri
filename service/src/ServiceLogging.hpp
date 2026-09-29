#pragma once

#include "wheelemu/Logging.hpp"

#include <string>

namespace wheelemu::service {

// Per-user log location: %LOCALAPPDATA%\wheelemu\logs, ~/.local/state/wheelemu (XDG) or ~/Library/Logs/wheelemu.
[[nodiscard]] std::string defaultLogFile();

// Sends SDL's own log messages (device and driver problems) into the "sdl" logger.
void routeSdlLogging();

// Writes version, platform and SDL details at startup so every log is self-describing.
void logStartupBanner(const std::string& configPath);

}  // namespace wheelemu::service
