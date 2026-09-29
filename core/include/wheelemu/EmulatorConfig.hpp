#pragma once

#include "wheelemu/Ffb.hpp"
#include "wheelemu/IniDocument.hpp"
#include "wheelemu/LogConfig.hpp"
#include "wheelemu/MapperConfig.hpp"
#include "wheelemu/WheelState.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wheelemu {

inline constexpr std::uint16_t kDefaultStatePort = 41329;

struct EmulatorConfig {
    std::string sourceDeviceMatch = "346e";     // case-insensitive part of the wheel's name or "vid:pid" (346e = Moza)
    std::string targetProfile = "g920";
    std::uint16_t statePort = kDefaultStatePort; // UDP loopback port the service publishes state on
    int loopHz = 1000;
    bool ffbInvert = false;                    // flip force direction if the wheel pulls the wrong way

    MapperConfig mapper;
    FfbSafetyConfig ffb;
    logging::LogConfig logging;
};

struct EmulatorConfigResult {
    EmulatorConfig config;
    std::vector<std::string> errors;            // unknown keys and invalid values; defaults are kept for these
};

[[nodiscard]] EmulatorConfigResult loadEmulatorConfig(const IniDocument& ini);

}  // namespace wheelemu
