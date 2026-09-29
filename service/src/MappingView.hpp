#pragma once

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/MapperConfig.hpp"
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

    void render(const RawInputState& raw, const WheelState& state, const TargetReport& report);

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

    std::uint64_t previousSourceButtons_ = 0;
    int lastPressedSource_ = kUnmapped;
};

}  // namespace wheelemu::service
