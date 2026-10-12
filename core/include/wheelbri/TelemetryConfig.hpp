#pragma once

#include <string>

namespace wheelbri {

// [telemetry] section of the service config; see wheelbri/Telemetry.hpp for what is recorded.
struct TelemetryConfig {
    bool enabled = true;
    int rateHz = 50;                   // at most this many rows per second (the loop runs faster)
    bool onlyChanges = true;           // write a row only when an input or output value changed
    int heartbeatSeconds = 60;         // with onlyChanges: a row at least this often anyway (0 = never)
    float minChange = 0.0005F;         // smallest axis change that counts, in -1..1 units (0.0005 = 0.23 degrees
                                       // of steering at 900 degrees); smaller moves are sensor noise
    int maxFileMb = 500;               // per file (one file per day); recording stops, with a note, when reached
    std::string directory;             // empty: the service's log folder
};

}  // namespace wheelbri
