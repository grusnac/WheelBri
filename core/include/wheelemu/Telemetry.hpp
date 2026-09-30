#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/TelemetryConfig.hpp"
#include "wheelemu/WheelState.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wheelemu {

// Local recording of everything that passes through the service, for tuning and debugging after a drive.
// Nothing leaves the PC: two CSV files per session in the log folder.
//
//  telemetry-<stamp>.csv   one row every 1/rate_hz: the physical wheel (raw axes, buttons, hat), the mapped
//                          state, what the emulated wheel reports to the game, the game's force feedback per
//                          slot (requested -> sent to the wheel) and loop timing.
//  ffb-<stamp>.csv         one row per force feedback change the game sent (every SlotState / StopAll
//                          packet, keep-alives are only counted), with every effect parameter.
//
// Both start with '#' comment lines (version, device, config) followed by a header row.

// One row of telemetry-*.csv. `ffb` is null when game force feedback is off.
struct TelemetrySample {
    std::uint64_t nowNs = 0;
    std::uint32_t sequence = 0;        // StatePacket sequence sent to the shim
    RawInputState raw;
    WheelState state;
    TargetReport report;
    const FfbController* ffb = nullptr;
    double periodUs = 0.0;             // last loop period and the work inside it
    double workUs = 0.0;
};

class TelemetryRecorder {
public:
    // Creates both files. `header` lines are written as '#' comments (device, config, ...).
    // Returns nullptr and fills `error` when the folder or files cannot be created.
    [[nodiscard]] static std::unique_ptr<TelemetryRecorder> open(const TelemetryConfig& config,
                                                                 const std::filesystem::path& directory,
                                                                 const std::string& stamp,
                                                                 const std::vector<std::string>& header,
                                                                 std::string& error);
    ~TelemetryRecorder();

    TelemetryRecorder(const TelemetryRecorder&) = delete;
    TelemetryRecorder& operator=(const TelemetryRecorder&) = delete;

    // True when a sample row is due (call every loop tick; cheap).
    [[nodiscard]] bool due(std::uint64_t nowNs) const;
    void writeSample(const TelemetrySample& sample);

    // Every packet the service received from the game shim (keep-alives only counted).
    void writeFfbPacket(std::uint64_t nowNs, const FfbPacket& packet);
    // A datagram on the force feedback port that was not a valid packet.
    void noteMalformedFfb(std::uint64_t nowNs, std::size_t bytes);

    [[nodiscard]] const std::filesystem::path& samplesPath() const { return samplesPath_; }
    [[nodiscard]] const std::filesystem::path& ffbPath() const { return ffbPath_; }
    [[nodiscard]] std::uint64_t rowsWritten() const { return rows_; }
    [[nodiscard]] std::uint64_t ffbRowsWritten() const { return ffbRows_; }

private:
    struct File {
        std::FILE* handle = nullptr;
        std::uint64_t bytes = 0;
        bool full = false;
    };

    TelemetryRecorder(const TelemetryConfig& config, std::filesystem::path samplesPath, std::filesystem::path ffbPath);
    void write(File& file, const std::string& line);
    void flushIfDue(std::uint64_t nowNs);

    TelemetryConfig config_;
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

// "20260930-142501" from a wall-clock time, local time zone. Used in the file names.
[[nodiscard]] std::string telemetryStamp(std::time_t time);

// Deletes the oldest telemetry sessions in `directory` so that at most `keep` remain (a session is the
// telemetry-<stamp>.csv / ffb-<stamp>.csv pair). Returns the number of files deleted.
std::size_t pruneTelemetry(const std::filesystem::path& directory, int keep);

// Column names of telemetry-*.csv, in order (also written as the header row).
[[nodiscard]] std::vector<std::string> telemetryColumns();

}  // namespace wheelemu
