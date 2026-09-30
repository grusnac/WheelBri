#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/LocalDate.hpp"
#include "wheelemu/TelemetryConfig.hpp"
#include "wheelemu/WheelState.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelemu {

// One row of telemetry-*.csv. `ffb` is null when game force feedback is off.
struct TelemetrySample {
    std::uint64_t nowNs = 0;           // monotonic clock
    std::uint32_t sequence = 0;        // StatePacket sequence sent to the shim
    RawInputState raw;
    WheelState state;
    TargetReport report;
    const FfbController* ffb = nullptr;
    double periodUs = 0.0;             // last loop period and the work inside it
    double workUs = 0.0;
};

// Local recording of everything that passes through the service, for tuning and debugging after a drive.
// Nothing leaves the PC. One pair of CSV files per day (local time), shared by every session of that day:
//
//  telemetry-YYYY-MM-DD.csv   one row every 1/rate_hz: the physical wheel (raw axes, buttons, hat), the mapped
//                             state, what the emulated wheel reports to the game, the game's force feedback per
//                             slot (requested -> sent to the wheel) and loop timing.
//  ffb-YYYY-MM-DD.csv         one row per force feedback change the game sent (every SlotState / StopAll
//                             packet, keep-alives are only counted), with every effect parameter.
//
// A new file starts with '#' comment lines and the column header row. Every session then appends a
// "# session ..." block (version, device, config) followed by its rows; a session running past midnight
// continues in the next day's files. `time` is the local time of day, `t_ms` counts from the session start.
class TelemetryRecorder {
public:
    // `wallNow` is the current wall-clock time (the session start). `header` lines are written as '#'
    // comments at the start of the session. Returns nullptr and fills `error` when the folder or files
    // cannot be created.
    [[nodiscard]] static std::unique_ptr<TelemetryRecorder> open(const TelemetryConfig& config,
                                                                 const std::filesystem::path& directory,
                                                                 WallClock::time_point wallNow,
                                                                 const std::vector<std::string>& header,
                                                                 std::string& error);
    ~TelemetryRecorder();

    TelemetryRecorder(const TelemetryRecorder&) = delete;
    TelemetryRecorder& operator=(const TelemetryRecorder&) = delete;

    // True when a sample row is due (call every loop tick; cheap). `nowNs` is a monotonic clock.
    [[nodiscard]] bool due(std::uint64_t nowNs) const;
    void writeSample(const TelemetrySample& sample);

    // Every packet the service received from the game shim (keep-alives only counted).
    void writeFfbPacket(std::uint64_t nowNs, const FfbPacket& packet);
    // A datagram on the force feedback port that was not a valid packet.
    void noteMalformedFfb(std::uint64_t nowNs, std::size_t bytes);

    // The files being written now (they change at midnight).
    [[nodiscard]] const std::filesystem::path& samplesPath() const { return samplesPath_; }
    [[nodiscard]] const std::filesystem::path& ffbPath() const { return ffbPath_; }
    [[nodiscard]] const std::string& day() const { return day_; }
    // Rows written by this session (all days).
    [[nodiscard]] std::uint64_t rowsWritten() const { return rows_; }
    [[nodiscard]] std::uint64_t ffbRowsWritten() const { return ffbRows_; }

private:
    struct File {
        std::FILE* handle = nullptr;
        std::uint64_t bytes = 0;
        bool full = false;
    };

    TelemetryRecorder(const TelemetryConfig& config, std::filesystem::path directory,
                      WallClock::time_point sessionStart, std::vector<std::string> header);
    bool openDay(const std::string& day, bool continued, std::string& error);
    void closeFiles();
    void write(File& file, const std::string& line);
    void flushIfDue(std::uint64_t nowNs);
    // Starts the clock on the first call, moves to the next day's files when midnight has passed, and
    // returns the wall-clock time of `nowNs`.
    WallClock::time_point advance(std::uint64_t nowNs);
    void appendTimes(std::string& line, WallClock::time_point wall, std::uint64_t nowNs) const;

    TelemetryConfig config_;
    std::filesystem::path directory_;
    std::vector<std::string> header_;
    WallClock::time_point sessionStart_;
    std::string day_;
    WallClock::time_point dayStart_;
    WallClock::time_point nextDayStart_;
    std::filesystem::path samplesPath_;
    std::filesystem::path ffbPath_;
    File samples_;
    File ffbEvents_;
    std::uint64_t periodNs_;
    std::optional<std::uint64_t> startNs_;
    std::optional<std::uint64_t> lastSampleNs_;
    std::uint64_t lastFlushNs_ = 0;
    std::uint64_t rows_ = 0;
    std::uint64_t ffbRows_ = 0;
    std::uint64_t keepAlives_ = 0;
    std::string line_;                 // reused row buffer
};

// Column names of telemetry-*.csv, in order (also written as the header row).
[[nodiscard]] std::vector<std::string> telemetryColumns();

}  // namespace wheelemu
