#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/Ffb.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/GameTuning.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/LogConfig.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/Retention.hpp"
#include "wheelbri/TelemetryConfig.hpp"
#include "wheelbri/WheelState.hpp"

#include <cstdint>
#include <span>
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
    // [emulator] trueforce_probe = true: the virtual G923 also gets its TrueForce interface, which only logs what it is
    // sent (TrueForce.hpp). Off by default: a game that sees TrueForce may move its force feedback there.
    bool trueForceProbe = false;
    int stateRateHz = 500;                       // at most this many state packets a second, only when it changed
    bool ffbInvert = false;                    // flip force direction if the wheel pulls the wrong way
    // [ffb] centring_spring = 0..100: the target's centring spring in percent, 0 = off (CentringSpring.hpp). Played
    // only for a target that had one (DeviceIdentity::centringSpring: the G25 and G27).
    float centringSpringPercent = 0.0F;
    // [buttons] order = driver | native: the order the target's buttons reach the game in (ButtonOrder,
    // DeviceProfile.hpp); native only for a target that has one (the G25 and G27), others ignore it.
    ButtonOrder buttonOrder = ButtonOrder::Driver;

    // [emulator] from_rig = true: the steering, pedals, D-pad and core buttons (CoreButton) a line here leaves out
    // come from the Rig (what each control "is", AutoMap.hpp: applyRig fills them in), and target buttons with no line
    // are unmapped. false (a mapping written before 2026-10-23): a button with no line is the wheel's button of the
    // same number, the steering the wheel's axis 0, and the Rig is not read.
    bool fromRig = false;
    AutoInputs autoInputs;   // with fromRig: the inputs with no line (MapperConfig.hpp)

    MapperConfig mapper;
    // [game_rotation] <profile id> = 180 | off: the steering rotation while that game runs, instead of the
    // game profile's own (GameRotation.hpp). A game left out uses its profile's, if it has one.
    GameRotations gameRotations;
    // [game_settings] <profile id>.<setting id> = 0..1: the user's value for a game's own setting that WheelBri sets
    // in the game's settings file (GameSettings.hpp). A setting left out has its game profile's value.
    GameSettingOverrides gameSettings;
    // [game_ffb_gain] <profile id> = 0.25..4: the game's pushing forces times this while it runs, instead of its game
    // profile's [ffb] gain (GameSettings.hpp).
    GameFfbGains gameFfbGains;
    // [game_tuning] <game id>.<key> = value: the user's own values per game (GameTuning.hpp), for a built-in profile
    // or a game the user added.
    GameTunings gameTunings;
    FfbSafetyConfig ffb;
    logging::LogConfig logging;
    TelemetryConfig telemetry;
    RetentionConfig retention;
};

struct EmulatorConfigResult {
    EmulatorConfig config;
    std::vector<std::string> errors;            // unknown keys and invalid values; defaults are kept for these
};

// `userGames`: the games the user added (GameTuning.hpp's parseUserGames), which [game_tuning] may name besides the
// built-in profiles.
[[nodiscard]] EmulatorConfigResult loadEmulatorConfig(const IniDocument& ini,
                                                      std::span<const GameProfile> userGames = {});

}  // namespace wheelbri
