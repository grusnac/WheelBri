#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/MapperConfig.hpp"
#include "wheelemu/Telemetry.hpp"
#include "wheelemu/WheelState.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace wheelemu::service {

// Live window for `--ui`: shows every axis and button of the physical wheel next to what the emulated
// wheel reports for it, so the mapping in the config can be checked by moving each control.
// Read-only: the mapping itself is edited in the INI file.
// What the window shows about game force feedback. `controller` is null when it is off or unavailable;
// `status` then says why.
struct FfbDisplay {
    const FfbController* controller = nullptr;
    std::string status;
    bool invert = false;
    std::uint64_t nowNs = 0;
};

class MappingView {
public:
    // Opens the window. Returns nullptr (and logs why) if SDL's video subsystem or the window cannot be created.
    [[nodiscard]] static std::unique_ptr<MappingView> open(std::string sourceName, int sourceAxes, int sourceButtons,
                                                           const IDeviceProfile& profile, const MapperConfig& mapper);
    ~MappingView();

    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;

    // Handles window events. Returns false once the user closed the window or pressed Esc.
    bool pumpEvents();

    // Where the service writes its log and telemetry, shown at the bottom of the window (F opens the folder).
    // `logFile` empty: logging to the console only. `telemetry` null: `telemetryStatus` says why.
    void setFiles(std::string logFile, const TelemetryRecorder* telemetry, std::string telemetryStatus);

    void render(const RawInputState& raw, const WheelState& state, const TargetReport& report,
                const FfbDisplay& ffb = {});

private:
    MappingView(SDL_Window* window, SDL_Renderer* renderer, std::string sourceName, int sourceAxes,
                int sourceButtons, const IDeviceProfile& profile, const MapperConfig& mapper);

    void drawHeader();
    void drawAxes(const RawInputState& raw, const WheelState& state, const TargetReport& report);
    void drawRawAxes(const RawInputState& raw);
    void drawHat(std::int8_t sourceHat, std::int8_t targetHat);
    void drawTargetButtons(std::uint32_t targetButtons);
    void drawSourceButtons(std::uint64_t sourceButtons);
    void drawFooter();
    void drawFfb(const FfbDisplay& ffb);
    void drawFiles();
    void openFilesFolder() const;
    void trackLastPressed(std::uint64_t sourceButtons);

    SDL_Window* window_;
    SDL_Renderer* renderer_;
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

    std::string logFile_;
    const TelemetryRecorder* telemetry_ = nullptr;
    std::string telemetryStatus_ = "off";

    std::uint64_t previousSourceButtons_ = 0;
    int lastPressedSource_ = kUnmapped;
};

}  // namespace wheelemu::service
