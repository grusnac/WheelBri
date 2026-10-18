#pragma once

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

// The user's own values for one game (the Game profiles tab), laid over the Mapping tab and the game's profile while
// that game runs. Kept per mapping (my-<target>.ini), since buttons and axes belong to a target:
//   [game_tuning]
//   nfs2015.throttle_axis = slider     ; the axis the game reads the pedal from: x y z rx ry rz slider (clutch: none)
//   nfs2015.brake_invert = true        ; pedals: <pedal>_invert = true|false (how they feel: the rig's own software)
//   nfsunbound.ffb_constant = 3        ; force feedback per kind of effect, 0..4: constant (steering force),
//                                      ;   periodic (vibration), spring, damper, friction, inertia
//   nfsunbound.ffb_strength = 0.6      ; the overall strength instead of [ffb] master_gain, 0..1
//   nfsunbound.ffb_cap = 0.6           ; lower only: never above [ffb] max_magnitude (and max_slew_per_second)
//   nfsunbound.ffb_slew = 30
//   nfsunbound.ffb_fade_s = 2          ; instead of [ffb] frozen_force_s, 0..60
//   nfsunbound.ffb_invert = false      ; instead of [ffb] invert / game_effects
//   nfsunbound.ffb_effects = true
//   nfsheat.button_4 = 13              ; target button 4 <- this source while the game runs (-1: nothing)
// A key left out keeps what the Mapping tab (or the game's profile) has. Every force still goes through
// FfbSafetyLimiter, the watchdog and the frozen-push fade: a game's values only scale what the game asks within the
// limits, and can lower the limits, never raise them.
namespace wheelbri {

// The kinds of effect a game's force feedback gain is set for, in the order of the window and the keys.
enum class FfbGroup : std::uint8_t { Constant, Periodic, Spring, Damper, Friction, Inertia };
inline constexpr std::size_t kFfbGroupCount = 6;
// "constant", "periodic", ...: the key after ffb_.
[[nodiscard]] std::string_view ffbGroupKey(FfbGroup group);
// The group an effect type belongs to (sine, square, triangle and the saws are periodic).
[[nodiscard]] FfbGroup ffbGroupOf(FfbEffectType type);

inline constexpr float kGameEffectGainMax = 4.0F;   // 0 (that kind off) .. 4 (four times what the game asks)

// Pedal 0 throttle, 1 brake, 2 clutch.
inline constexpr std::array<std::string_view, 3> kTuningPedals{"throttle", "brake", "clutch"};

struct GameTuning {
    std::array<std::optional<ObjectGuid>, 3> pedalAxes;   // the axis the game reads each pedal from
    std::array<std::optional<bool>, 3> pedalInvert;
    std::optional<float> ffbStrength;
    std::array<std::optional<float>, kFfbGroupCount> ffbGains;
    std::optional<float> ffbCap;
    std::optional<float> ffbSlew;
    std::optional<float> ffbFadeSeconds;
    std::optional<bool> ffbInvert;
    std::optional<bool> ffbEffects;
    std::map<int, SourceRef> buttons;   // target button -> its source while the game runs

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

// The force feedback settings while a game runs: the Mapping tab's ([ffb]), with the game's tuning laid over them.
struct GameFfb {
    FfbSafetyConfig safety;                       // strength, cap, slew and fade in force
    std::array<float, kFfbGroupCount> gains{};    // per kind of effect, 0..kGameEffectGainMax
    bool invert = false;
    bool effects = true;
    friend bool operator==(const GameFfb&, const GameFfb&) = default;
};
// `base` and `baseInvert` / `baseEffects`: the mapping's [ffb]. `game` (may be null: no game running, or one whose
// profile does not apply) gives its profile's [ffb] gain to constant and periodic effects; `legacyGain` (the older
// [game_ffb_gain]) replaces it; `tuning` (may be null) has the last word. The cap and the slew limit only ever go down.
[[nodiscard]] GameFfb resolveGameFfb(const FfbSafetyConfig& base, bool baseInvert, bool baseEffects,
                                     const GameProfile* game, std::optional<float> legacyGain,
                                     const GameTuning* tuning);

// The mapping while the game runs: `base` with its pedals' invert and its buttons as `tuning` says. A button whose
// source is on an accessory `base` does not read (the session opened only those) is left as it is, and said in
// `problems` (may be null).
[[nodiscard]] MapperConfig tunedMapper(const MapperConfig& base, const GameTuning& tuning,
                                       std::vector<std::string>* problems);
// True when `tuning` changes the mapping (pedal invert, buttons): a tuned mapper is needed.
[[nodiscard]] bool changesMapping(const GameTuning& tuning);
// The axes the game reads its pedals from: `layout` with the tuning's axes laid over it.
[[nodiscard]] JoystickLayout tunedLayout(JoystickLayout layout, const GameTuning& tuning);

// What a session does for the game that runs (service GameWatch): its built-in profile when it fits the target, then
// the user's own values for it. Pure, so it is tested here.
struct GameSessionConfig {
    std::string target;              // the session's target profile ("g920")
    bool routePedals = true;         // the virtual device: the service routes the pedals (the in-game shim does not)
    FfbSafetyConfig ffb;             // the mapping's [ffb]
    bool ffbInvert = false;
    bool gameFfb = true;
    GameFfbGains legacyGains;        // [game_ffb_gain]
    GameTunings tunings;             // [game_tuning]
};
struct GameDecision {
    PedalRoute route;                // straight through unless the game's pedals need routing
    bool unroutable = false;         // the game's pedal axes are not the device's: left as they are
    GameFfb ffb;
    const GameTuning* tuning = nullptr;   // the user's values for the game (in `config.tunings`), or null
};
// `game` null: no game runs. `profileApplies`: its built-in values fit the target (profileFitsTarget).
[[nodiscard]] GameDecision decideGame(const GameProfile* game, bool profileApplies, const GameSessionConfig& config);

// Games the user added (the Game profiles tab's Add game), kept in games.ini next to app.ini:
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
