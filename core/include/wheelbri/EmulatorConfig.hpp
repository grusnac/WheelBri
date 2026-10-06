#pragma once

#include "wheelbri/Ffb.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LogConfig.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/Retention.hpp"
#include "wheelbri/TelemetryConfig.hpp"
#include "wheelbri/WheelState.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wheelbri {

inline constexpr std::uint16_t kDefaultStatePort = 41329;
inline constexpr std::uint16_t kDefaultFfbPort = 41330;

struct EmulatorConfig {
    std::string sourceDeviceMatch;              // case-insensitive part of the wheel's name or "vid:pid"; empty = the rig's wheel
    std::string targetProfile = "g920";
    std::uint16_t statePort = kDefaultStatePort; // UDP loopback port the service publishes state on
    std::uint16_t ffbPort = kDefaultFfbPort;     // UDP loopback port the service receives game force feedback on
    bool gameFfb = true;                         // play the game's force feedback (sent by the shim)
    int loopHz = 1000;
    int stateRateHz = 500;                       // at most this many state packets a second, only when it changed
    bool ffbInvert = false;                    // flip force direction if the wheel pulls the wrong way

    MapperConfig mapper;
    FfbSafetyConfig ffb;
    logging::LogConfig logging;
    TelemetryConfig telemetry;
    RetentionConfig retention;
};

struct EmulatorConfigResult {
    EmulatorConfig config;
    std::vector<std::string> errors;            // unknown keys and invalid values; defaults are kept for these
};

[[nodiscard]] EmulatorConfigResult loadEmulatorConfig(const IniDocument& ini);

}  // namespace wheelbri
