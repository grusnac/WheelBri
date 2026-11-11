// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/GameProfiles.hpp"

#include "wheelbri/AntiDeadZone.hpp"
#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/IniDocument.hpp"

#include <algorithm>
#include <mutex>

namespace wheelbri {

namespace {

// config/games/*.ini, embedded by core/CMakeLists.txt; the last entry is an empty sentinel.
constexpr GameProfileFile kFiles[] = {
#include "GameProfileFiles.inc"
    {"", ""},
};

constexpr std::size_t kMaxLabel = 20;   // the window shows it after "Running 1:23, "
constexpr std::size_t kMaxNote = 160;   // one line of the window

std::string lower(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// [game] ways: "virtual", "shim" or both, comma-separated, any spacing, each once. False (the profile untouched) for
// an empty list, another word or one said twice.
bool parseWays(const std::string_view text, GameProfile& profile) {
    bool virtualDevice = false;
    bool shim = false;
    std::string_view rest = text;
    while (true) {
        const std::size_t comma = rest.find(',');
        std::string_view word = rest.substr(0, comma);
        while (!word.empty() && (word.front() == ' ' || word.front() == '\t')) word.remove_prefix(1);
        while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) word.remove_suffix(1);
        bool& seen = word == "virtual" ? virtualDevice : shim;
        if ((word != "virtual" && word != "shim") || seen) return false;
        seen = true;
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    profile.virtualDevice = virtualDevice;
    profile.shim = shim;
    return true;
}

bool validSettingId(const std::string_view id) {
    if (id.empty() || id.size() > 24) return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
    }
    return true;
}

bool validSettingName(const std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return false;
        }
    }
    return true;
}

// "WheelSpringMagnitude 0.8" or "WheelMaxRotation steering".
std::optional<GameSetting> parseSetting(const std::string& id, const std::string_view text) {
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos) return std::nullopt;
    std::string_view rest = text.substr(space + 1);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    GameSetting setting{id, std::string(text.substr(0, space)), std::nullopt};
    if (!validSettingName(setting.name)) return std::nullopt;
    if (rest == "steering") return setting;
    const std::optional<float> value = tuning::parseNumber(rest);
    if (!value || *value < kGameSettingMin || *value > kGameSettingMax) return std::nullopt;
    setting.value = *value;
    return setting;
}

// Relative, / separated, no empty, . or .. parts, no drive or backslash.
bool validSettingsFile(const std::string_view file) {
    if (file.empty() || file.front() == '/' || file.find('\\') != std::string_view::npos ||
        file.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= file.size()) {
        const std::size_t slash = file.find('/', start);
        const std::string_view part = file.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                                          : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

struct Loaded {
    std::vector<GameProfile> profiles;
    std::vector<std::string> problems;
};

const Loaded& loaded() {
    static Loaded result;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const GameProfileFile& file : builtInGameProfileFiles()) {
            ParsedGameProfile parsed = parseGameProfile(file.id, file.text);
            for (const std::string& problem : parsed.problems) {
                result.problems.push_back(std::string(file.id) + ".ini: " + problem);
            }
            if (!parsed.profile) continue;
            bool duplicate = false;
            for (const GameProfile& other : result.profiles) {
                duplicate = duplicate ||
                            (other.program == parsed.profile->program && other.marker == parsed.profile->marker);
            }
            if (duplicate) {
                result.problems.push_back(std::string(file.id) + ".ini: program " + parsed.profile->program +
                                          " already has a profile (with the same marker): this one is not used");
                continue;
            }
            result.profiles.push_back(std::move(*parsed.profile));
        }
    });
    return result;
}

}  // namespace

std::span<const GameProfileFile> builtInGameProfileFiles() {
    return std::span<const GameProfileFile>(kFiles, std::size(kFiles) - 1);
}

ParsedGameProfile parseGameProfile(const std::string_view id, const std::string_view text) {
    ParsedGameProfile result;
    IniDocument::ParseResult ini = IniDocument::parse(text);
    result.problems = std::move(ini.errors);
    GameProfile profile;
    profile.id = std::string(id);
    bool waysSet = false;   // [game] ways was there (even when it could not be read)
    bool anyTarget = false;   // [game] target = any

    for (const auto& [section, keys] : ini.document.sections()) {
        if (section == "game") {
            for (const auto& [key, value] : keys) {
                if (key == "label") {
                    profile.label = value;
                } else if (key == "program") {
                    profile.program = lower(value);
                } else if (key == "marker") {
                    profile.marker = lower(value);
                    if (profile.marker.empty() || profile.marker == "." || profile.marker == ".." ||
                        profile.marker.find_first_of("/\\:") != std::string::npos) {
                        result.problems.push_back("[game] marker = '" + value + "': expected a file name (dimap.dll)");
                    }
                } else if (key == "target") {
                    // any: every target (a profile that changes nothing a target decides, e.g. only [pedals] combined).
                    profile.target = lower(value);
                    anyTarget = profile.target == "any";
                    if (anyTarget) profile.target.clear();
                } else if (key == "ways") {
                    waysSet = true;
                    if (!parseWays(value, profile)) {
                        result.problems.push_back("[game] ways = '" + value +
                                                  "': expected virtual, shim or both (virtual, shim)");
                    }
                } else if (key == "ways_note") {
                    profile.waysNote = value;
                } else if (key == "not_for") {
                    profile.notFor.clear();
                    std::string_view rest = value;
                    while (!rest.empty()) {
                        const std::size_t comma = rest.find(',');
                        std::string one = lower(rest.substr(0, comma));
                        one.erase(0, one.find_first_not_of(' '));
                        one.erase(one.find_last_not_of(' ') + 1);
                        if (makeProfile(one) == nullptr) {
                            result.problems.push_back("[game] not_for = '" + value + "': '" + one +
                                                      "' is not a target (" + std::string(kKnownProfiles) + ")");
                        } else {
                            profile.notFor.push_back(one);
                        }
                        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
                    }
                } else if (key == "not_for_note") {
                    profile.notForNote = value;
                } else {
                    result.problems.push_back("[game] " + key + ": unknown key");
                }
            }
        } else if (section == "pedals") {
            for (const auto& [key, value] : keys) {
                if (key == "combined") {
                    if (value == "true") {
                        profile.combinedPedals = true;
                    } else if (value == "false") {
                        profile.combinedPedals = false;
                    } else {
                        result.problems.push_back("[pedals] combined = '" + value + "': expected true or false");
                    }
                    continue;
                }
                std::optional<ObjectGuid>* field = key == "throttle_axis" ? &profile.throttleAxis
                                                   : key == "brake_axis"  ? &profile.brakeAxis
                                                   : key == "clutch_axis" ? &profile.clutchAxis
                                                                          : nullptr;
                if (field == nullptr) {
                    result.problems.push_back("[pedals] " + key + ": unknown key");
                } else if (const auto axis = parseAxisName(value)) {
                    *field = *axis;
                } else {
                    result.problems.push_back("[pedals] " + key + " = '" + value +
                                              "': expected x, y, z, rx, ry, rz, slider or none");
                }
            }
        } else if (section == "steering") {
            for (const auto& [key, value] : keys) {
                if (key == "rotation_deg") {
                    const auto degrees = tuning::parseNumber(value);
                    const std::string problem = degrees ? tuning::rotationProblem(*degrees) : "expected a number";
                    if (problem.empty()) {
                        profile.rotationDeg = *degrees;
                    } else {
                        result.problems.push_back("[steering] rotation_deg = '" + value + "': " + problem);
                    }
                } else if (key == "note") {
                    profile.rotationNote = value;
                } else if (key == "anti_deadzone") {
                    const auto percent = tuning::parseNumber(value);
                    if (percent && antiDeadZoneUsable(*percent)) {
                        profile.antiDeadZonePercent = *percent;
                    } else {
                        result.problems.push_back("[steering] anti_deadzone = '" + value +
                                                  "': expected a number from 0 to " +
                                                  tuning::formatNumber(kAntiDeadZoneMaxPercent));
                    }
                } else {
                    result.problems.push_back("[steering] " + key + ": unknown key");
                }
            }
        } else if (section == "game_settings") {
            for (const auto& [key, value] : keys) {
                if (key == "file") {
                    profile.settingsFile = value;
                } else if (key == "note") {
                    profile.settingsNote = value;
                } else if (!validSettingId(key)) {
                    result.problems.push_back("[game_settings] " + key +
                                              ": a setting's id is lower case letters and _ (e.g. spring)");
                } else if (const std::optional<GameSetting> setting = parseSetting(key, value)) {
                    profile.settings.push_back(*setting);
                } else {
                    result.problems.push_back("[game_settings] " + key + " = '" + value +
                                              "': expected the game's name for it and a number from " +
                                              tuning::formatNumber(kGameSettingMin) + " to " +
                                              tuning::formatNumber(kGameSettingMax) + " or steering");
                }
            }
        } else {
            result.problems.push_back("[" + section +
                                      "]: unknown section (only [game], [pedals], [steering] and [game_settings]; a "
                                      "game's force feedback is the game's and the wheel's own app's to set)");
        }
    }
    if (profile.label.empty()) {
        result.problems.push_back("[game] label is missing (e.g. NFS 2015)");
    } else if (profile.label.size() > kMaxLabel) {
        result.problems.push_back("[game] label '" + profile.label + "': at most " + std::to_string(kMaxLabel) +
                                  " characters (the window shows it next to Start)");
    } else if (lower(profile.label).rfind("need for speed", 0) == 0) {
        result.problems.push_back("[game] label '" + profile.label + "': write NFS, e.g. NFS 2015 or NFS Heat");
    }
    if (profile.program.empty() || programName(profile.program) != profile.program) {
        result.problems.push_back("[game] program must be the game's program file name, e.g. nfs16.exe");
    }
    if (profile.target.empty() && !anyTarget) result.problems.push_back("[game] target is missing (e.g. g920 or any)");
    if (anyTarget && (profile.throttleAxis || profile.brakeAxis || profile.clutchAxis)) {
        result.problems.push_back("[pedals] *_axis need one target: the axes are that target's");
    }
    if (profile.notFor.empty() != profile.notForNote.empty()) {
        result.problems.push_back("[game] not_for and not_for_note go together: the targets and what goes wrong");
    } else if (profile.notForNote.size() > kMaxNote) {
        result.problems.push_back("[game] not_for_note: at most " + std::to_string(kMaxNote) + " characters");
    } else if (std::find(profile.notFor.begin(), profile.notFor.end(), profile.target) != profile.notFor.end()) {
        result.problems.push_back("[game] not_for: the target it was checked with (" + profile.target +
                                  ") cannot be one it does not support");
    }
    // The note says why the game does not work the other way: only with ways, and only when it leaves one out.
    const bool restricted = !profile.virtualDevice || !profile.shim;
    if (waysSet && !restricted && !profile.waysNote.empty()) {
        result.problems.push_back("[game] ways_note: only for a game that does not work both ways");
    } else if (restricted != !profile.waysNote.empty()) {
        result.problems.push_back("[game] ways and ways_note go together: the ways and why not the other");
    } else if (profile.waysNote.size() > kMaxNote) {
        result.problems.push_back("[game] ways_note: at most " + std::to_string(kMaxNote) + " characters");
    }
    if (profile.rotationDeg.has_value() != !profile.rotationNote.empty()) {
        result.problems.push_back("[steering] rotation_deg and note go together: the advice and why");
    } else if (profile.rotationNote.size() > kMaxNote) {
        result.problems.push_back("[steering] note: at most " + std::to_string(kMaxNote) + " characters");
    }
    const bool anySettings =
        !profile.settingsFile.empty() || !profile.settingsNote.empty() || !profile.settings.empty();
    if (anySettings) {
        if (profile.settingsFile.empty() || profile.settingsNote.empty() || profile.settings.empty()) {
            result.problems.push_back("[game_settings] file, note and at least one setting go together");
        } else if (!validSettingsFile(profile.settingsFile)) {
            result.problems.push_back("[game_settings] file = '" + profile.settingsFile +
                                      "': a path under the Documents folder, / separated, no .. (e.g. "
                                      "Need For Speed(TM) Unbound/settings/PROFILEOPTIONS)");
        } else if (profile.settings.size() > kMaxGameSettings) {
            result.problems.push_back("[game_settings]: at most " + std::to_string(kMaxGameSettings) + " settings");
        } else if (profile.settingsNote.size() > kMaxNote) {
            result.problems.push_back("[game_settings] note: at most " + std::to_string(kMaxNote) + " characters");
        }
        for (std::size_t i = 0; i < profile.settings.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (profile.settings[i].name == profile.settings[j].name) {
                    result.problems.push_back("[game_settings] " + profile.settings[i].id + ": " +
                                              profile.settings[i].name + " is set twice");
                }
            }
        }
    }
    if (!isValid(overlay(layoutFor(profile.target, 0), profile))) {
        result.problems.push_back("[pedals]: two pedals on one axis, a pedal on x (the wheel), or throttle/brake none");
    }
    if (result.problems.empty()) result.profile = std::move(profile);
    return result;
}

std::span<const GameProfile> gameProfiles() { return loaded().profiles; }


const std::vector<std::string>& gameProfileProblems() { return loaded().problems; }

std::string programName(const std::string_view nameOrPath) {
    const std::size_t slash = nameOrPath.find_last_of("/\\");
    return lower(slash == std::string_view::npos ? nameOrPath : nameOrPath.substr(slash + 1));
}

namespace {

[[nodiscard]] bool markerFound(const GameProfile& profile, const std::span<const std::string> found) {
    return std::find(found.begin(), found.end(), profile.marker) != found.end();
}

}  // namespace

const GameProfile* gameProfileFor(const std::string_view nameOrPath, const std::span<const GameProfile> profiles,
                                  const std::span<const std::string> found) {
    const std::string name = programName(nameOrPath);
    if (name.empty()) return nullptr;
    for (const GameProfile& profile : profiles) {
        if (name == profile.program && !profile.marker.empty() && markerFound(profile, found)) return &profile;
    }
    for (const GameProfile& profile : profiles) {
        if (name == profile.program && profile.marker.empty()) return &profile;
    }
    return nullptr;
}

std::optional<std::string> targetAdvice(const GameProfile& game, const std::string_view target) {
    const std::string wanted = lower(target);
    if (std::find(game.notFor.begin(), game.notFor.end(), wanted) == game.notFor.end()) return std::nullopt;
    const auto upper = [](std::string text) {
        for (char& c : text) c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        return text;
    };
    std::string text = game.label + " does not support the " + upper(wanted) + ": " + game.notForNote + ".";
    if (!game.target.empty()) text += " Use the " + upper(game.target) + " for it (Mapping tab, Target).";
    return text;
}

std::optional<WayAdvice> wayAdvice(const GameProfile& game, const bool shim) {
    if (shim ? game.shim : game.virtualDevice) return std::nullopt;
    const std::string way = shim ? "Mode Shim" : "Mode Virtual";     // the session's, which fails
    const std::string need = shim ? "Mode Virtual" : "Mode Shim";    // the one it works with
    return WayAdvice{game.label + " needs " + need,
                     game.label + " does not work with " + way + " (" + game.waysNote + "). Close the game, Stop, set "
                     "Mode to " + (shim ? "Virtual" : "Shim") + " on the Mapping tab, and Start again."};
}

bool profileFitsTarget(const GameProfile& profile, const std::string_view targetProfile) {
    return profile.target.empty() || profile.target == lower(targetProfile);   // empty: a game the user added
}

JoystickLayout overlay(JoystickLayout base, const GameProfile& profile) {
    base.throttle = profile.throttleAxis.value_or(base.throttle);
    base.brake = profile.brakeAxis.value_or(base.brake);
    base.clutch = profile.clutchAxis.value_or(base.clutch);
    return base;
}

std::string describeOverlay(const GameProfile& profile) {
    std::string out;
    const auto add = [&](const char* key, const std::optional<ObjectGuid>& axis) {
        if (!axis) return;
        out += (out.empty() ? "" : ", ") + std::string(key) + " = " + std::string(axisName(*axis));
    };
    add("throttle_axis", profile.throttleAxis);
    add("brake_axis", profile.brakeAxis);
    add("clutch_axis", profile.clutchAxis);
    if (profile.combinedPedals) {
        out += (out.empty() ? "" : ", ") + std::string("combined = ") + (*profile.combinedPedals ? "true" : "false");
    }
    return out;
}

std::optional<PedalRoute> routePedals(const JoystickLayout& device, const JoystickLayout& game) {
    const std::array<ObjectGuid, 3> slots{device.throttle, device.brake, device.clutch};
    const std::array<ObjectGuid, 3> wanted{game.throttle, game.brake, game.clutch};
    PedalRoute route;
    for (std::size_t field = 0; field < 3; ++field) {
        bool found = false;
        for (std::size_t pedal = 0; pedal < 3 && !found; ++pedal) {
            if (slots[field] != ObjectGuid::Any && wanted[pedal] == slots[field]) {
                route.from[field] = static_cast<std::uint8_t>(pedal);
                found = true;
            }
        }
        if (!found) return std::nullopt;   // a device axis no pedal goes to, or one the device does not have
    }
    return route;
}

TargetReport applyPedalRoute(const TargetReport& report, const PedalRoute route) {
    if (route.from[0] > 2 || route.from[1] > 2 || route.from[2] > 2) return report;   // not a route: straight through
    const std::array<std::int32_t, 3> pedals{report.throttle, report.brake, report.clutch};
    TargetReport routed = report;
    routed.throttle = pedals[route.from[0]];
    routed.brake = pedals[route.from[1]];
    routed.clutch = pedals[route.from[2]];
    return routed;
}

std::vector<RunningGame> findRunningGames(const std::span<const RunningProgram> programs,
                                          const std::span<const GameProfile> profiles) {
    std::vector<std::optional<RunningGame>> byProfile(profiles.size());
    std::vector<bool> taken(programs.size(), false);
    const auto claim = [&](const std::size_t p) {
        const GameProfile& profile = profiles[p];
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const RunningProgram& program = programs[i];
            if (taken[i] || programName(program.name) != profile.program) continue;
            if (!profile.path.empty() && !program.path.empty() && !samePath(program.path, profile.path)) continue;
            if (!profile.marker.empty() && !markerFound(profile, program.found)) continue;
            taken[i] = true;
            byProfile[p] = RunningGame{&profile, program.pid, program.name};
            return;
        }
    };
    for (std::size_t p = 0; p < profiles.size(); ++p) {
        if (!profiles[p].path.empty()) claim(p);
    }
    for (std::size_t p = 0; p < profiles.size(); ++p) {
        if (profiles[p].path.empty() && !profiles[p].marker.empty()) claim(p);
    }
    for (std::size_t p = 0; p < profiles.size(); ++p) {
        if (profiles[p].path.empty() && profiles[p].marker.empty()) claim(p);
    }
    std::vector<RunningGame> found;
    for (const std::optional<RunningGame>& game : byProfile) {
        if (game) found.push_back(*game);
    }
    return found;
}

bool samePath(const std::string_view a, const std::string_view b) {
    const auto fold = [](const char c) {
        if (c == '/') return '\\';
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (fold(a[i]) != fold(b[i])) return false;
    }
    return true;
}

}  // namespace wheelbri
