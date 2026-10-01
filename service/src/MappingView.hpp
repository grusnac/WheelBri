#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/MapperConfig.hpp"

#include "Housekeeping.hpp"
#include "wheelemu/WheelState.hpp"

#include "UiWindow.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace wheelemu::service {

// Live window for `--ui`: shows every axis and button of the physical wheel next to what the emulated
// wheel reports for it, so the mapping in the config can be checked by moving each control.
// Read-only: the mapping itself is edited in the INI file. Drawn with the system's own window tools
// (UiWindow.hpp: Win32 on Windows, X11 on Linux).
// What the window shows about game force feedback. `controller` is null when it is off or unavailable;
// `status` then says why.
struct FfbDisplay {
    const FfbController* controller = nullptr;
    std::string status;
    bool invert = false;
    std::uint64_t nowNs = 0;
};

// The telemetry files as last seen by the loop (a copy: the recorder belongs to the loop's thread).
struct TelemetryFiles {
    bool on = false;                              // false: `FilesDisplay::telemetryStatus` says why
    std::filesystem::path samplesPath;
    std::filesystem::path ffbPath;
    std::uint64_t rows = 0;
    std::uint64_t ffbRows = 0;
};

// Where the service writes its logs and telemetry, and what happens to old files (bottom of the window).
struct FilesDisplay {
    std::filesystem::path logFolder;
    bool logToFile = true;                        // false: the log file could not be opened
    TelemetryFiles telemetry;
    std::string telemetryStatus;
    const Housekeeping* housekeeping = nullptr;   // owned by the window's thread
};

class MappingView {
public:
    // Opens the window. Returns nullptr (and logs why) if it cannot be created (no display, ...).
    [[nodiscard]] static std::unique_ptr<MappingView> open(std::string sourceName, int sourceAxes, int sourceButtons,
                                                           const IDeviceProfile& profile, const MapperConfig& mapper);
    ~MappingView();

    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;

    // Handles window events. Returns false once the user closed the window or pressed Esc.
    bool pumpEvents();

    // Shown at the bottom of the window; F opens the folder.
    void setFiles(FilesDisplay files) { files_ = std::move(files); }
    void setTelemetry(TelemetryFiles telemetry) { files_.telemetry = std::move(telemetry); }

    void render(const RawInputState& raw, const WheelState& state, const TargetReport& report,
                const FfbDisplay& ffb = {});

private:
    MappingView(std::unique_ptr<ui::UiWindow> window, std::string sourceName, int sourceAxes, int sourceButtons,
                const IDeviceProfile& profile, const MapperConfig& mapper);

    void drawHeader();
    void drawAxes(const RawInputState& raw, const WheelState& state, const TargetReport& report);
    void drawRawAxes(const RawInputState& raw);
    void drawHat(std::int8_t sourceHat, std::int8_t targetHat);
    void drawTargetButtons(std::uint32_t targetButtons);
    void drawSourceButtons(std::uint64_t sourceButtons);
    void drawFooter();
    void drawFfb(const FfbDisplay& ffb);
    void drawFfbUsage(const FfbController& controller, float top);
    void drawFiles();
    void openFilesFolder() const;
    void trackLastPressed(std::uint64_t sourceButtons);

    std::unique_ptr<ui::UiWindow> window_;
    std::string sourceName_;
    int sourceAxes_;
    int sourceButtons_;
    const IDeviceProfile& profile_;
    MapperConfig mapper_;
    std::string target_;            // short name of the emulated wheel ("G920")
    std::string pedalNote_;         // how the target encodes released / pressed pedals

    // sourceToTarget_[s] = first target button fed by source button s, or kUnmapped; count for duplicates.
    std::array<int, 64> sourceToTarget_{};
    std::array<int, 64> sourceTargetCount_{};
    // sourceHatDirection_[s] = D-pad direction ("up", "right", ...) source button s drives, or nullptr.
    std::array<const char*, 64> sourceHatDirection_{};

    FilesDisplay files_;
    // File names looked up at most once a second (they change at midnight).
    std::uint64_t filesCheckedMs_ = 0;
    std::string serviceLogName_;
    std::string gameLogName_;

    bool showFfbUsage_ = false;     // U switches the force feedback panel: live effects / usage since start

    std::uint64_t previousSourceButtons_ = 0;
    int lastPressedSource_ = kUnmapped;
};

}  // namespace wheelemu::service
