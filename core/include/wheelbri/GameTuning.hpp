// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/ButtonRef.hpp"
#include "wheelbri/EmulatedJoystick.hpp"
#include "wheelbri/Ffb.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameSettings.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/SourceRef.hpp"

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The user's own values for one game (the Games tab), laid over the Mapping tab and the game's profile while
// that game runs. Kept per mapping (my-<target>.ini), since buttons and axes belong to a target:
//   [game_tuning]
//   nfs2015.throttle_axis = slider     ; the axis the game reads the pedal from: x y z rx ry rz slider (clutch: none)
//   nfs2015.brake_invert = true        ; pedals: <pedal>_invert = true|false (how they feel: the rig's own software)
//   nfsheat.button_4 = 13              ; target button 4 <- this source while the game runs (-1: nothing)
//   nfsheat.button_4_or = 20, axis 3 > 50%   ; its secondary sources while the game runs, as [buttons] target_N_or
//                                      ;   (ButtonRef.hpp): instead of the Mapping tab's, not added to them (-1: none)
// A key left out keeps what the Mapping tab (or the game's profile) has. The older force feedback keys (ffb_strength,
// ffb_constant .. ffb_inertia, ffb_cap, ffb_slew, ffb_fade_s, ffb_invert, ffb_effects; until 2026-11-02) are read and
// ignored: how strong a game's force feedback is, is the game's and the wheel's own app's to set. The Live tab shows
// what a running game asks and what reaches the wheel (FfbGroups.hpp).
namespace wheelbri {

// Pedal 0 throttle, 1 brake, 2 clutch.
inline constexpr std::array<std::string_view, 3> kTuningPedals{"throttle", "brake", "clutch"};

struct GameTuning {
    std::array<std::optional<ObjectGuid>, 3> pedalAxes;   // the axis the game reads each pedal from
    std::array<std::optional<bool>, 3> pedalInvert;
    std::map<int, SourceRef> buttons;   // target button -> its source while the game runs
    // target button -> its secondary sources while the game runs (at most MapperConfig::kMaxSecondaries), instead of
    // the Mapping tab's; empty: none in this game. A button left out keeps the Mapping tab's.
    std::map<int, std::vector<ButtonRef>> secondaries;

    [[nodiscard]] bool empty() const;
    friend bool operator==(const GameTuning&, const GameTuning&) = default;
};

// By game id (a built-in profile's, or one the user added: UserGames).
using GameTunings = std::map<std::string, GameTuning>;

// One [game_tuning] line, `key` without the game ("throttle_axis"), into `tuning`. Empty when it was used, else why
// not ("expected a number from 0 to 4"); `tuning` is left as it was then. `maxButtons`: the target's button count.
[[nodiscard]] std::string applyTuningKey(GameTuning& tuning, std::string_view key, std::string_view value,
                                         std::size_t maxButtons);
// Every line of `tuning` as key (without the game) and value, in a fixed order: what writeMapping writes.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> tuningLines(const GameTuning& tuning);

// The mapping while the game runs: `base` with its pedals' invert, its buttons and their secondaries as `tuning` says.
// A button (or a button's secondaries) with a source on an accessory `base` does not read (the session opened only
// those) is left as it is, and said in `problems` (may be null).
[[nodiscard]] MapperConfig tunedMapper(const MapperConfig& base, const GameTuning& tuning,
                                       std::vector<std::string>* problems);
// True when `tuning` changes the mapping (pedal invert, buttons, secondaries): a tuned mapper is needed.
[[nodiscard]] bool changesMapping(const GameTuning& tuning);
// The axes the game reads its pedals from: `layout` with the tuning's axes laid over it.
[[nodiscard]] JoystickLayout tunedLayout(JoystickLayout layout, const GameTuning& tuning);

// What a session does for the game that runs (service GameWatch): its built-in profile when it fits the target, then
// the user's own values for it. Pure, so it is tested here.
struct GameSessionConfig {
    std::string target;              // the session's target profile ("g920")
    bool routePedals = true;         // the virtual device: the service routes the pedals (the in-game shim does not)
    GameTunings tunings;             // [game_tuning]
};
struct GameDecision {
    PedalRoute route;                // straight through unless the game's pedals need routing
    bool unroutable = false;         // the game's pedal axes are not the device's: left as they are
    const GameTuning* tuning = nullptr;   // the user's values for the game (in `config.tunings`), or null
};
// `game` null: no game runs. `profileApplies`: its built-in values fit the target (profileFitsTarget).
[[nodiscard]] GameDecision decideGame(const GameProfile* game, bool profileApplies, const GameSessionConfig& config);

// Games the user added (the Games tab's Add game), kept in games.ini next to app.ini:
//   [game.excalibur]
//   label = Excalibur
//   program = excalibur.exe
// They fit every target and change nothing until the user sets values for them ([game_tuning]).
struct UserGamesResult {
    std::vector<GameProfile> games;
    std::vector<std::string> problems;
};
[[nodiscard]] UserGamesResult parseUserGames(std::string_view text);
[[nodiscard]] std::string formatUserGames(std::span<const GameProfile> games);
// An id for a new game from its program ("NeedForSpeedHeat.exe" -> "needforspeedheat"): lower case letters, digits and
// _ only; empty when nothing is left. Not checked against other ids.
[[nodiscard]] std::string gameIdFor(std::string_view program);

}  // namespace wheelbri
