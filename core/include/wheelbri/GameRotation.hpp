#pragma once

#include "wheelbri/GameProfiles.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

// The steering rotation used while a game with a profile runs. A game profile can name one ([steering] rotation_deg:
// NFS Payback 180): while that game runs the service steers with it instead of the mapping's own, the wheel and the
// game's full lock both at that number (1:1, so the wheel's end stop is the game's full lock and there is no travel
// past it). The user's mapping can change it per game, or turn it off ([game_rotation] in the config, set in the Games
// tab):
//   [game_rotation]
//   nfspayback = 180        ; wheel and game 180
//   nfspayback = 360/180    ; wheel 360, game 180 (faster steering, but the wheel turns on past the game's full lock)
//   nfspayback = off        ; the mapping's own rotation in this game too
// WheelBri never reads or sets the wheel's own software: the wheel's number must be what that software is set to.
namespace wheelbri {

struct SteeringRotation {
    float wheelDeg = 900.0F;   // what the wheel's own software is set to
    float gameDeg = 900.0F;    // the game's full lock

    [[nodiscard]] float ratio() const { return gameDeg > 0.0F ? wheelDeg / gameDeg : 1.0F; }
    friend bool operator==(const SteeringRotation&, const SteeringRotation&) = default;
};

// One game's line in [game_rotation]: off, or a rotation.
struct GameRotationOverride {
    bool off = false;
    SteeringRotation rotation;

    friend bool operator==(const GameRotationOverride&, const GameRotationOverride&) = default;
};

// By game profile id ("nfspayback").
using GameRotations = std::map<std::string, GameRotationOverride>;

// "180", "360/180", "off" (any case, spaces allowed); nullopt otherwise or out of range (AxisTuning.hpp).
[[nodiscard]] std::optional<GameRotationOverride> parseGameRotation(std::string_view text);
// The other way: "180" when both are the same, "360/180", "off".
[[nodiscard]] std::string formatGameRotation(const GameRotationOverride& value);
// "180 deg", "wheel 360 / game 180 deg".
[[nodiscard]] std::string describeRotation(const SteeringRotation& rotation);

// What steers while `game` runs: the user's line for it, else the profile's rotation (wheel = game); nullopt: the
// mapping's own (no rotation in the profile, or the user's line says off).
[[nodiscard]] std::optional<SteeringRotation> rotationWhileRunning(const GameProfile& game,
                                                                   const GameRotations& overrides);

}  // namespace wheelbri
