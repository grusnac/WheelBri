#pragma once

#include <string>

namespace wheelemu {

// [telemetry] section of the service config; see wheelemu/Telemetry.hpp for what is recorded.
struct TelemetryConfig {
    bool enabled = true;
    int rateHz = 50;                   // sample rows per second (the loop runs faster; rows are decimated)
    int maxFileMb = 200;               // per file; recording stops (with a note in the file) when reached
    int keepSessions = 20;             // older sessions in the folder are deleted when a new one starts
    std::string directory;             // empty: the service's log folder
};

}  // namespace wheelemu
