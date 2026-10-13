#pragma once

#include "wheelbri/GameProfiles.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

// The steering rotation used while a game with a profile runs. A game profile can name one ([steering] rotation_deg:
// NFS Payback 180): while that game runs, that is the rotation to play it with instead of the mapping's own
// ([steering] target_rotation_deg). The axis always goes through 1:1, so the wheel's end stop is the game's full lock:
// the wheel's own software (Pit House for Moza) must be set to the same number, by the user (WheelBri never reads or
// sets it); the app and the log say so. A game whose own settings follow the steering rotation gets it written there
// (GameSettings.hpp). The user's mapping can change it per game, or turn it off ([game_rotation] in the config, set in
// the Games tab):
//   [game_rotation]
//   nfspayback = 180        ; 180 in this game
//   nfspayback = off        ; the mapping's own rotation in this game too
// Older files may say "360/180" (a wheel rotation, then the game's: before 2026-10-13): read as the game's, 180.
namespace wheelbri {

// One game's line in [game_rotation]: off, or a rotation in degrees.
struct GameRotationOverride {
    bool off = false;
    float deg = 900.0F;

    friend bool operator==(const GameRotationOverride&, const GameRotationOverride&) = default;
};

// By game profile id ("nfspayback").
using GameRotations = std::map<std::string, GameRotationOverride>;

// "180", "off" (any case, spaces allowed), and the older "360/180" (its second number); nullopt otherwise or out of
// range (AxisTuning.hpp).
[[nodiscard]] std::optional<GameRotationOverride> parseGameRotation(std::string_view text);
// The other way: "180", "off".
[[nodiscard]] std::string formatGameRotation(const GameRotationOverride& value);
// "180 deg".
[[nodiscard]] std::string describeRotation(float deg);

// The rotation while `game` runs: the user's line for it, else the profile's; nullopt: the mapping's own (no rotation
// in the profile, or the user's line says off).
[[nodiscard]] std::optional<float> rotationWhileRunning(const GameProfile& game, const GameRotations& overrides);

}  // namespace wheelbri
