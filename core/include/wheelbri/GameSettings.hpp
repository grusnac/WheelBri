#pragma once

#include "wheelbri/GameProfiles.hpp"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A game's own settings that WheelBri sets for it (a game profile's [game_settings], GameProfiles.hpp). NFS Unbound
// keeps its wheel force feedback settings in its settings file (in Documents: Need For Speed(TM) Unbound, settings,
// PROFILEOPTIONS) and shows none of them in its menus; at 0.5 each (its defaults) its force feedback is very light
// (2026-10-12: with WheelFFBMagnitude 1.0 and WheelSpringMagnitude 1.0 its centring spring went from 0 most of the
// time to 75-99%, its idle spring to full strength, its steering force and rumble up by half). The service sets them
// while the game is closed (the game reads the file when it starts and writes its own values back when it ends); the
// user changes them in the Mapping tab (PER GAME), saved as
//   [game_settings]
//   nfsunbound.spring = 0.8        ; <game profile id>.<setting id> = kGameSettingMin..kGameSettingMax
// A setting left out has its profile's value. "steering" settings (WheelMaxRotation) get the steering rotation the
// game is played with: its PER GAME rotation ([game_rotation], GameRotation.hpp, set in the Mapping tab as for NFS
// Payback), else the mapping's own; so changing that rotation changes the game's file too.
namespace wheelbri {

// By game profile id, then setting id: the user's own values.
using GameSettingOverrides = std::map<std::string, std::map<std::string, float>>;

// The user's own force feedback gain per game ([game_ffb_gain] <game profile id> = kFfbGainMin..kFfbGainMax, the
// Mapping tab's PER GAME rows): instead of the profile's [ffb] gain (FfbController::setGameGain).
using GameFfbGains = std::map<std::string, float>;
// The gain while `game` runs: the user's, else the profile's, else 1 (the game's forces as it asks).
[[nodiscard]] float gameFfbGain(const GameProfile& game, const GameFfbGains& gains);

// The value `setting` gets: the user's, else the profile's; nullopt for a "steering" setting.
[[nodiscard]] std::optional<float> gameSettingValue(const GameProfile& game, const GameSetting& setting,
                                                    const GameSettingOverrides& overrides);
// True when one of the game's settings is "steering" (the game keeps its own steering rotation): its rotation is set
// per game in the Mapping tab like a profile's [steering] rotation.
[[nodiscard]] bool hasSteeringSetting(const GameProfile& game);
// The profile's setting with this id, or null.
[[nodiscard]] const GameSetting* findGameSetting(const GameProfile& game, std::string_view id);
// "spring" -> "spring", "force_feedback" -> "force feedback": for the window.
[[nodiscard]] std::string gameSettingLabel(std::string_view id);

// One value to write: the game's name for it and its text as the game writes it ("0.800000", "360.000000").
struct GameSettingText {
    std::string name;
    std::string text;
    friend bool operator==(const GameSettingText&, const GameSettingText&) = default;
};
// What the game's file should hold: every setting of `game`, "steering" ones as `steeringDeg`.
[[nodiscard]] std::vector<GameSettingText> gameSettingTexts(const GameProfile& game,
                                                            const GameSettingOverrides& overrides, float steeringDeg);
// A number as the game writes it: six decimals ("0.800000").
[[nodiscard]] std::string formatGameSettingText(float value);

// Frostbite settings files (NFS Unbound's PROFILEOPTIONS): "FBCHUNKS", then entries of a 4-byte type, a 4-byte length
// and the name with a 0 after it, a 4-byte length and the value as text with a 0 after it (little-endian lengths that
// count the 0). Only what is needed to change a value in place is read: each name is found by its own entry (its
// length fields must match, its value must be short printable text), and must be there exactly once.
//
// `bytes` with each value replaced. A value is only ever replaced by text of the same length, so nothing else in the
// file moves (its layout is not known beyond the entries); anything else is a problem and then nothing is changed at
// all: `problems` says why, `bytes` is empty.
struct GameSettingsEdit {
    struct Change {
        std::string name;
        std::string before;
        std::string after;
    };
    std::string bytes;
    std::vector<Change> changes;          // only the values that differ
    std::vector<std::string> problems;
};
[[nodiscard]] GameSettingsEdit editFrostbiteSettings(std::string_view bytes, std::span<const GameSettingText> values);
// The value of `name` in a Frostbite settings file, or nullopt (not a Frostbite file, no such entry, or more than one).
[[nodiscard]] std::optional<std::string> frostbiteSetting(std::string_view bytes, std::string_view name);

}  // namespace wheelbri
