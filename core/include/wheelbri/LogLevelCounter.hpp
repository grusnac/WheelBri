#pragma once

#include "wheelbri/LiveLog.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace wheelbri {

// The warnings and errors in one log file (the Logs tab's Helper row turns to the warning colour when today's helper
// log has any). Lines are read as LiveLog reads them (parseLogLine), the whole file, a part per poll.
struct LogLevelCount {
    std::uint64_t warnings = 0;
    std::uint64_t errors = 0;   // error and critical
    std::string lastTime;       // of the latest warning or error ("14:03:21.120"; empty when the line had none)
    std::string lastMessage;    // and its text

    [[nodiscard]] bool any() const { return warnings + errors > 0; }
};

class LogLevelCounter {
public:
    // Read per poll at most, so a huge file cannot stall the window (the rest comes at the next polls).
    static constexpr std::uintmax_t kMaxReadBytes = 1024 * 1024;

    // The file to count (a new path starts over at zero; empty: none).
    void setFile(const std::filesystem::path& path);

    // Reads what was added since the last poll. A file that got shorter (replaced) or went away starts over at zero.
    // True when the count changed.
    bool poll();

    [[nodiscard]] const LogLevelCount& count() const { return count_; }
    [[nodiscard]] const std::filesystem::path& file() const { return path_; }

private:
    void startOver();
    void take(const std::string& line);

    std::filesystem::path path_;
    std::uintmax_t offset_ = 0;
    std::string partial_;   // an unfinished last line
    LogLevelCount count_;
};

}  // namespace wheelbri
