#pragma once

#include <string>

namespace wheelemu {

// [telemetry] section of the service config; see wheelemu/Telemetry.hpp for what is recorded.
struct TelemetryConfig {
    bool enabled = true;
    int rateHz = 50;                   // sample rows per second (the loop runs faster; rows are decimated)
    int maxFileMb = 500;               // per file (one file per day); recording stops, with a note, when reached
    std::string directory;             // empty: the service's log folder
};

}  // namespace wheelemu
