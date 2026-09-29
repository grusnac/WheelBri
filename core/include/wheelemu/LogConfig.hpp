#pragma once

#include <cstddef>
#include <string>

namespace wheelemu::logging {

// Kept free of spdlog so configuration code does not pull the logging headers in.
struct LogConfig {
    std::string level = "info";                  // trace | debug | info | warn | error | critical | off
    std::string filePath;                        // empty = application default location
    std::size_t maxFileSizeBytes = 10 * 1024 * 1024;
    std::size_t maxFiles = 5;                    // rotated files kept besides the active one
    bool console = true;                         // coloured stderr output (off inside the game process)
    std::string flushLevel = "warn";             // messages at or above this level are written to disk at once
    bool periodicFlush = true;                   // background flush every second; off where no thread may be started
};

[[nodiscard]] bool isValidLevel(const std::string& level);

}  // namespace wheelemu::logging
