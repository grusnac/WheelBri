// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// The live log of the --ui window's Logs tab: the newest lines of every component's log file (the service,
// the game shim, the drivers, the bridge), merged as they are written. Not on the service loop.

enum class LiveLogLevel { Trace, Debug, Info, Warn, Error, Unknown };

struct LiveLogLine {
    int source = 0;                     // which file it came from (the index given to LiveLog::setFile)
    std::string time;                   // "22:49:33.488", or empty if the line has no time stamp
    LiveLogLevel level = LiveLogLevel::Unknown;
    std::string logger;                 // spdlog's component ("service.ffb"); empty for the drivers and the bridge
    std::string message;
};

// One line as the components write it:
//   spdlog (service, shim): "[2026-10-02 22:49:33.488] [info ] [service       ] [tid 23616 ] text  (File.cpp:40)"
//   drivers, bridge:        "[2026-10-02 22:49:33.488] text"
// Anything else is kept whole as the message.
[[nodiscard]] LiveLogLine parseLogLine(std::string_view line, int source);

// Follows a set of files, one per component, and keeps the newest lines of all of them.
class LiveLog {
public:
    // At most this many lines are kept (the oldest go first).
    static constexpr std::size_t kMaxLines = 5000;
    // When a file is first seen, only its last part is read.
    static constexpr std::uintmax_t kInitialTailBytes = 48 * 1024;
    // Read per file per poll at most, so a huge file cannot stall the window.
    static constexpr std::uintmax_t kMaxReadBytes = 512 * 1024;
    // A line longer than this is shown cut.
    static constexpr std::size_t kMaxLineBytes = 16 * 1024;

    // Which file component `source` writes now (a new path, e.g. the next day's, starts over at its tail).
    // An empty path stops following it.
    void setFile(int source, const std::filesystem::path& path);

    // Reads what was added to the files since the last poll. Returns true if lines were added.
    bool poll();

    [[nodiscard]] const std::deque<LiveLogLine>& lines() const { return lines_; }
    // Grows by one for every line ever added (so a viewer can tell how many arrived since it last looked).
    [[nodiscard]] std::uint64_t added() const { return added_; }

private:
    struct File {
        int source = 0;
        std::filesystem::path path;
        std::uintmax_t offset = 0;
        bool started = false;           // the initial tail was read
        std::string partial;            // an unfinished last line, completed by the next poll
    };

    void add(LiveLogLine line);

    std::vector<File> files_;
    std::deque<LiveLogLine> lines_;
    std::uint64_t added_ = 0;
};

}  // namespace wheelbri
