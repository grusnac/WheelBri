#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace wheelbri {

// Where a DailyLogFile writes: one file per day (the WheelBri Helper's opens it as the connected user). A fake in
// the tests.
class LogTarget {
public:
    virtual ~LogTarget() = default;
    // Opens (creates or appends to) the file of `date` ("YYYY-MM-DD"). False when it cannot.
    [[nodiscard]] virtual bool open(const std::string& date) = 0;
    // Writes one whole line (with its line end) to the open file. False when it could not.
    [[nodiscard]] virtual bool write(std::string_view line) = 0;
    virtual void close() = 0;
};

// How long DailyLogFile keeps lines without a file, and how often it tries to open one again.
struct DailyLogFileSettings {
    std::size_t maxPending = 2000;
    std::uint64_t retryMs = 60'000;
};

// A log written to a daily file that may not be there yet: the WheelBri Helper's (HelperLog.cpp), which starts at
// boot, before anyone it could write for has connected.
// - Without a target, or while its file cannot be opened, lines wait in memory: at most `maxPending`, the oldest
//   dropped first and counted; the count is written as a line of its own before the rest when a file opens.
// - A new day closes the file and opens the next one at once.
// - A file that could not be opened, or stopped taking lines, is tried again on a later line, at most once every
//   `retryMs` (nothing is lost meanwhile: the lines wait).
// Not thread-safe: the caller holds a lock.
class DailyLogFile {
public:
    using Settings = DailyLogFileSettings;
    // Makes the line saying how many lines were dropped (in the log's own format, with its line end).
    using NoteLine = std::function<std::string(const std::string& message)>;

    explicit DailyLogFile(Settings settings = {}, NoteLine noteLine = {});
    ~DailyLogFile();

    DailyLogFile(const DailyLogFile&) = delete;
    DailyLogFile& operator=(const DailyLogFile&) = delete;

    // Writes into `target` from now on (the previous one is closed): opens the file of `date` and writes what waited.
    // A null target only closes the previous one.
    // `where` names the place `target` writes to (the Helper: the user's logs folder). When it is the place the
    // current target already writes to, `target` is dropped and the current one kept: the same file goes on, and a
    // caller saying "here" again (the app's Hello every 5 minutes) makes no new, empty file. An empty `where` always
    // takes the new target.
    void attach(std::unique_ptr<LogTarget> target, const std::string& date, std::uint64_t nowMs,
                const std::string& where = {});

    // One whole line, written on `date` at `nowMs` (a steady clock).
    void write(std::string line, const std::string& date, std::uint64_t nowMs);

    [[nodiscard]] bool hasTarget() const { return target_ != nullptr; }
    [[nodiscard]] const std::string& where() const { return where_; }
    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] const std::string& fileDate() const { return fileDate_; }
    [[nodiscard]] std::size_t pending() const { return pending_.size(); }
    [[nodiscard]] std::size_t dropped() const { return dropped_; }

private:
    void tryOpen(const std::string& date, std::uint64_t nowMs);
    [[nodiscard]] bool writeWaiting();   // the dropped count, then the pending lines; false when the file failed
    void lost(std::uint64_t nowMs);      // the file failed: closed, tried again later
    void keep(std::string line);

    Settings settings_;
    NoteLine noteLine_;
    std::unique_ptr<LogTarget> target_;
    std::string where_;   // what attach was told the current target writes to
    bool open_ = false;
    std::string fileDate_;
    std::uint64_t lastTryMs_ = 0;
    std::deque<std::string> pending_;
    std::size_t dropped_ = 0;
};

}  // namespace wheelbri
