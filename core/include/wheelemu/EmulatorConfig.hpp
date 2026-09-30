#pragma once

#include "wheelemu/Ffb.hpp"
#include "wheelemu/IniDocument.hpp"
#include "wheelemu/LogConfig.hpp"
#include "wheelemu/MapperConfig.hpp"
#include "wheelemu/TelemetryConfig.hpp"
#include "wheelemu/WheelState.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wheelemu {

inline constexpr std::uint16_t kDefaultStatePort = 41329;
inline constexpr std::uint16_t kDefaultFfbPort = 41330;

struct EmulatorConfig {
    std::string sourceDeviceMatch = "346e";     // case-insensitive part of the wheel's name or "vid:pid" (346e = Moza)
    std::string targetProfile = "g920";
    std::uint16_t statePort = kDefaultStatePort; // UDP loopback port the service publishes state on
    std::uint16_t ffbPort = kDefaultFfbPort;     // UDP loopback port the service receives game force feedback on
    bool gameFfb = true;                         // play the game's force feedback (sent by the shim)
    int loopHz = 1000;
    bool ffbInvert = false;                    // flip force direction if the wheel pulls the wrong way

    MapperConfig mapper;
    FfbSafetyConfig ffb;
    logging::LogConfig logging;
    TelemetryConfig telemetry;
};

struct EmulatorConfigResult {
    EmulatorConfig config;
    std::vector<std::string> errors;            // unknown keys and invalid values; defaults are kept for these
};

[[nodiscard]] EmulatorConfigResult loadEmulatorConfig(const IniDocument& ini);

}  // namespace wheelemu
