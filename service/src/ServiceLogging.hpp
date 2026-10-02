#pragma once

#include "wheelemu/Logging.hpp"

#include <string>

namespace wheelemu::service {

// Per-user log location: %LOCALAPPDATA%\wheelemu\logs, ~/.local/state/wheelemu (XDG) or ~/Library/Logs/wheelemu.
[[nodiscard]] std::string defaultLogFile();

// Writes the version and platform at startup so every log is self-describing.
void logStartupBanner(const std::string& configPath);

}  // namespace wheelemu::service
