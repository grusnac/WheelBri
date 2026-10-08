#pragma once

#include <cstddef>
#include <string>

namespace wheelbri::logging {

// Kept free of spdlog so configuration code does not pull the logging headers in.
struct LogConfig {
    std::string level = "warn";                  // trace | debug | info | warn | error | critical | off
    std::string filePath;                        // empty = application default location; the date is added to
                                                 // the name, a new file every day ("x.log" -> "x-2026-09-30.log")
    bool console = true;                         // coloured stderr output (off inside the game process)
    std::string flushLevel = "warn";             // messages at or above this level are written to disk at once
    bool periodicFlush = true;                   // background flush every second; off where no thread may be started
};

[[nodiscard]] bool isValidLevel(const std::string& level);

}  // namespace wheelbri::logging
