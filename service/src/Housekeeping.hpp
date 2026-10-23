#pragma once

#include "wheelbri/LocalDate.hpp"
#include "wheelbri/Retention.hpp"

#include "Platform.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {

// A compressor found on this PC. Tools that make one archive per day (7z, zip) and tools that compress each
// file on its own (xz, zstd, gzip: "telemetry-2026-09-20-01.log" -> "telemetry-2026-09-20-01.log.xz").
struct Archiver {
    enum class Kind { SevenZip, Xz, Zstd, WindowsZip, InfoZip, Gzip };
    Kind kind;
    std::filesystem::path executable;

    [[nodiscard]] const char* name() const;
    [[nodiscard]] bool onePerDay() const { return kind == Kind::SevenZip || kind == Kind::WindowsZip || kind == Kind::InfoZip; }
};

// Best available compressor for `preference` ("auto" or one of wheelbri::kArchivers). "auto" tries, in order
// of how small they make log files: 7-Zip (7z, LZMA2 ultra), xz, zstd, zip (Windows' built-in
// tar.exe, or Info-ZIP), gzip. Looks on PATH and in the usual 7-Zip install folders.
[[nodiscard]] std::optional<Archiver> findArchiver(const std::string& preference);

// Applies [retention] to the log and telemetry folders: at start-up and again after every midnight.
// Deleting happens at once; archiving runs one compressor process at a time in the background (one CPU
// thread, pumped from the service loop), and deletes the originals only after the compressor succeeded.
class Housekeeping {
public:
    Housekeeping(RetentionConfig config, std::vector<std::filesystem::path> folders);
    // With `archiver` used as it is, never searched for (the tests: Windows' tar.exe on any computer).
    Housekeeping(RetentionConfig config, std::vector<std::filesystem::path> folders, Archiver archiver);
    ~Housekeeping();

    Housekeeping(const Housekeeping&) = delete;
    Housekeeping& operator=(const Housekeeping&) = delete;

    // Looks for files older than the limit and deletes them or queues them for archiving.
    void run(WallClock::time_point now);
    // Call regularly: starts the next archive job and collects finished ones. Cheap when idle.
    void pump();

    // One line for the window: what happens to old files and what is going on right now.
    [[nodiscard]] std::string status() const;
    [[nodiscard]] bool failed() const { return failed_; }

private:
    struct Job {
        std::string day;
        std::vector<std::filesystem::path> inputs;
        std::vector<std::filesystem::path> outputs;
        std::vector<std::string> args;
    };

    void deleteOld(const std::string& cutoff);
    void queueArchives(const std::string& cutoff);
    [[nodiscard]] Job makeJob(const std::string& day, const std::filesystem::path& folder,
                              std::vector<std::filesystem::path> files) const;
    void start(Job job);
    void finish(int exitCode);
    void abortRunning();

    RetentionConfig config_;
    std::vector<std::filesystem::path> folders_;
    std::optional<Archiver> archiver_;
    bool archiverSearched_ = false;
    std::deque<Job> queue_;
    std::optional<Job> running_;
    std::unique_ptr<platform::ChildProcess> process_;
    std::uint64_t daysArchived_ = 0;
    std::uint64_t filesDeleted_ = 0;
    bool failed_ = false;
    std::string lastProblem_;
};

}  // namespace wheelbri::service
