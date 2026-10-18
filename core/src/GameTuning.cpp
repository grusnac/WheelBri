#include "wheelbri/GameTuning.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/IniDocument.hpp"

#include <algorithm>
#include <charconv>

namespace wheelbri {

namespace {

constexpr std::array<std::string_view, kFfbGroupCount> kGroupKeys{"constant", "periodic", "spring",
                                                                   "damper",   "friction", "inertia"};

std::optional<bool> parseBool(const std::string_view text) {
    if (text == "true" || text == "on" || text == "yes" || text == "1") return true;
    if (text == "false" || text == "off" || text == "no" || text == "0") return false;
    return std::nullopt;
}

std::string range(const float low, const float high) {
    return "expected a number from " + tuning::formatNumber(low) + " to " + tuning::formatNumber(high);
}

// A number in [low, high] into `out`; why not otherwise.
std::string number(std::optional<float>& out, const std::string_view value, const float low, const float high) {
    const std::optional<float> parsed = tuning::parseNumber(value);
    // Written as "in range" so NaN is refused too.
    if (!parsed || !(*parsed >= low && *parsed <= high)) return range(low, high);
    out = *parsed;
    return {};
}

std::string boolean(std::optional<bool>& out, const std::string_view value) {
    const std::optional<bool> parsed = parseBool(value);
    if (!parsed) return "expected true or false";
    out = *parsed;
    return {};
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

std::string_view ffbGroupKey(const FfbGroup group) { return kGroupKeys[static_cast<std::size_t>(group)]; }

FfbGroup ffbGroupOf(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return FfbGroup::Constant;
        case FfbEffectType::Spring: return FfbGroup::Spring;
        case FfbEffectType::Damper: return FfbGroup::Damper;
        case FfbEffectType::Friction: return FfbGroup::Friction;
        case FfbEffectType::Inertia: return FfbGroup::Inertia;
        case FfbEffectType::Sine:
        case FfbEffectType::Square:
        case FfbEffectType::Triangle:
        case FfbEffectType::SawtoothUp:
        case FfbEffectType::SawtoothDown: return FfbGroup::Periodic;
    }
    return FfbGroup::Constant;
}

bool GameTuning::empty() const { return *this == GameTuning{}; }

std::string applyTuningKey(GameTuning& tuning, const std::string_view key, const std::string_view value,
                           const std::size_t maxButtons) {
    for (std::size_t p = 0; p < kTuningPedals.size(); ++p) {
        const std::string pedal(kTuningPedals[p]);
        if (key == pedal + "_axis") {
            const std::optional<ObjectGuid> axis = parseAxisName(lower(value));
            const bool clutch = p == 2;
            if (!axis || (*axis == ObjectGuid::Any && !clutch)) {
                return clutch ? "expected x, y, z, rx, ry, rz, slider or none"
                              : "expected x, y, z, rx, ry, rz or slider";
            }
            tuning.pedalAxes[p] = *axis;
            return {};
        }
        if (key == pedal + "_invert") return boolean(tuning.pedalInvert[p], value);
    }
    if (key == "ffb_strength") return number(tuning.ffbStrength, value, 0.0F, 1.0F);
    for (std::size_t g = 0; g < kFfbGroupCount; ++g) {
        if (key == "ffb_" + std::string(kGroupKeys[g])) {
            return number(tuning.ffbGains[g], value, 0.0F, kGameEffectGainMax);
        }
    }
    if (key == "ffb_cap") return number(tuning.ffbCap, value, 0.0F, 1.0F);
    if (key == "ffb_slew") return number(tuning.ffbSlew, value, 0.0F, 1000.0F);
    if (key == "ffb_fade_s") return number(tuning.ffbFadeSeconds, value, 0.0F, 60.0F);
    if (key == "ffb_invert") return boolean(tuning.ffbInvert, value);
    if (key == "ffb_effects") return boolean(tuning.ffbEffects, value);
    if (key.substr(0, 7) == "button_") {
        const std::string_view digits = key.substr(7);
        int target = -1;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), target);
        if (ec != std::errc() || end != digits.data() + digits.size() || target < 0 ||
            static_cast<std::size_t>(target) >= maxButtons) {
            return "no such button on the target (0 to " + std::to_string(maxButtons == 0 ? 0 : maxButtons - 1) + ")";
        }
        const std::optional<SourceRef> source = parseSource(value);
        if (!source) return "expected a button: 2, -1 (nothing) or 044f:b696/2";
        tuning.buttons[target] = *source;
        return {};
    }
    return "unknown key";
}

std::vector<std::pair<std::string, std::string>> tuningLines(const GameTuning& tuning) {
    std::vector<std::pair<std::string, std::string>> lines;
    const auto flag = [](const bool on) { return std::string(on ? "true" : "false"); };
    for (std::size_t p = 0; p < kTuningPedals.size(); ++p) {
        const std::string pedal(kTuningPedals[p]);
        if (tuning.pedalAxes[p]) lines.emplace_back(pedal + "_axis", std::string(axisName(*tuning.pedalAxes[p])));
        if (tuning.pedalInvert[p]) lines.emplace_back(pedal + "_invert", flag(*tuning.pedalInvert[p]));
    }
    if (tuning.ffbStrength) lines.emplace_back("ffb_strength", tuning::formatNumber(*tuning.ffbStrength));
    for (std::size_t g = 0; g < kFfbGroupCount; ++g) {
        if (tuning.ffbGains[g]) {
            lines.emplace_back("ffb_" + std::string(kGroupKeys[g]), tuning::formatNumber(*tuning.ffbGains[g]));
        }
    }
    if (tuning.ffbCap) lines.emplace_back("ffb_cap", tuning::formatNumber(*tuning.ffbCap));
    if (tuning.ffbSlew) lines.emplace_back("ffb_slew", tuning::formatNumber(*tuning.ffbSlew));
    if (tuning.ffbFadeSeconds) lines.emplace_back("ffb_fade_s", tuning::formatNumber(*tuning.ffbFadeSeconds));
    if (tuning.ffbInvert) lines.emplace_back("ffb_invert", flag(*tuning.ffbInvert));
    if (tuning.ffbEffects) lines.emplace_back("ffb_effects", flag(*tuning.ffbEffects));
    for (const auto& [target, source] : tuning.buttons) {
        lines.emplace_back("button_" + std::to_string(target), formatSource(source));
    }
    return lines;
}

GameFfb resolveGameFfb(const FfbSafetyConfig& base, const bool baseInvert, const bool baseEffects,
                       const GameProfile* game, const std::optional<float> legacyGain, const GameTuning* tuning) {
    GameFfb out;
    out.safety = base;
    out.invert = baseInvert;
    out.effects = baseEffects;
    out.gains.fill(1.0F);
    // The profile's (or the older [game_ffb_gain]) gain is for the pushing forces: constant and periodic.
    const float push = legacyGain.value_or(game != nullptr ? game->ffbGain.value_or(1.0F) : 1.0F);
    out.gains[static_cast<std::size_t>(FfbGroup::Constant)] = push;
    out.gains[static_cast<std::size_t>(FfbGroup::Periodic)] = push;
    if (tuning == nullptr) return out;
    if (tuning->ffbStrength) out.safety.masterGain = std::clamp(*tuning->ffbStrength, 0.0F, 1.0F);
    for (std::size_t g = 0; g < kFfbGroupCount; ++g) {
        if (tuning->ffbGains[g]) out.gains[g] = std::clamp(*tuning->ffbGains[g], 0.0F, kGameEffectGainMax);
    }
    // Lower only: a game's values never raise a limit above the mapping's.
    if (tuning->ffbCap) out.safety.maxMagnitude = std::min(base.maxMagnitude, std::max(0.0F, *tuning->ffbCap));
    if (tuning->ffbSlew) {
        out.safety.maxSlewPerSecond = std::min(base.maxSlewPerSecond, std::max(0.0F, *tuning->ffbSlew));
    }
    if (tuning->ffbFadeSeconds) out.safety.frozenForceSeconds = std::clamp(*tuning->ffbFadeSeconds, 0.0F, 60.0F);
    if (tuning->ffbInvert) out.invert = *tuning->ffbInvert;
    if (tuning->ffbEffects) out.effects = *tuning->ffbEffects;
    return out;
}

MapperConfig tunedMapper(const MapperConfig& base, const GameTuning& tuning, std::vector<std::string>* problems) {
    MapperConfig out = base;
    PedalConfig* const pedals[3] = {&out.throttle, &out.brake, &out.clutch};
    for (std::size_t p = 0; p < 3; ++p) {
        if (tuning.pedalInvert[p]) pedals[p]->invert = *tuning.pedalInvert[p];
    }
    for (const auto& [target, source] : tuning.buttons) {
        if (target < 0 || static_cast<std::size_t>(target) >= MapperConfig::kMaxTargetButtons) continue;
        int device = 0;
        if (!source.device.empty()) {
            device = -1;
            for (std::size_t a = 0; a < out.accessoryCount(); ++a) {
                if (out.accessories[a] == source.device) device = static_cast<int>(a + 1);
            }
        }
        if (device < 0) {
            if (problems != nullptr) {
                problems->push_back("button " + std::to_string(target) + " <- " + formatSource(source) +
                                    ": that device is not in the mapping, so it is not read; the button stays as "
                                    "mapped");
            }
            continue;
        }
        const auto t = static_cast<std::size_t>(target);
        out.buttonMap[t] = source.mapped() ? source.index : kUnmapped;
        out.buttonDevice[t] = source.mapped() ? device : 0;
    }
    return out;
}

bool changesMapping(const GameTuning& tuning) {
    for (std::size_t p = 0; p < 3; ++p) {
        if (tuning.pedalInvert[p]) return true;
    }
    return !tuning.buttons.empty();
}

JoystickLayout tunedLayout(JoystickLayout layout, const GameTuning& tuning) {
    if (tuning.pedalAxes[0]) layout.throttle = *tuning.pedalAxes[0];
    if (tuning.pedalAxes[1]) layout.brake = *tuning.pedalAxes[1];
    if (tuning.pedalAxes[2]) layout.clutch = *tuning.pedalAxes[2];
    return layout;
}

GameDecision decideGame(const GameProfile* const game, const bool profileApplies, const GameSessionConfig& config) {
    GameDecision decision;
    const GameProfile* const profile = game != nullptr && profileApplies ? game : nullptr;
    if (game != nullptr) {
        if (const auto found = config.tunings.find(game->id); found != config.tunings.end()) {
            decision.tuning = &found->second;
        }
    }
    // Pedals: the profile's layout over the device's, then the user's axes; routed only for the virtual device.
    const JoystickLayout device = layoutFor(config.target, 0);
    JoystickLayout wanted = profile != nullptr ? overlay(device, *profile) : device;
    if (decision.tuning != nullptr) wanted = tunedLayout(wanted, *decision.tuning);
    if (config.routePedals) {
        if (const std::optional<PedalRoute> routed = routePedals(device, wanted)) {
            decision.route = *routed;
        } else {
            decision.unroutable = true;
        }
    }
    std::optional<float> legacy;
    if (game != nullptr) {
        if (const auto found = config.legacyGains.find(game->id); found != config.legacyGains.end()) {
            legacy = found->second;
        }
    }
    decision.ffb = resolveGameFfb(config.ffb, config.ffbInvert, config.gameFfb, profile,
                                  profile != nullptr || decision.tuning != nullptr ? legacy : std::nullopt,
                                  decision.tuning);
    return decision;
}

UserGamesResult parseUserGames(const std::string_view text) {
    UserGamesResult result;
    const IniDocument::ParseResult parsed = IniDocument::parse(text);
    result.problems = parsed.errors;
    for (const auto& [section, keys] : parsed.document.sections()) {
        if (section.rfind("game.", 0) != 0) {
            result.problems.push_back("[" + section + "]: expected [game.<id>]");
            continue;
        }
        GameProfile game;
        game.id = section.substr(5);
        game.target.clear();   // every target
        for (const auto& [key, value] : keys) {
            if (key == "label") {
                game.label = value;
            } else if (key == "program") {
                game.program = programName(value);
            } else {
                result.problems.push_back("[" + section + "] " + key + ": unknown key");
            }
        }
        if (game.id.empty() || gameIdFor(game.id) != game.id) {
            result.problems.push_back("[" + section + "]: the id must be lower case letters, digits and _");
        } else if (game.label.empty() || game.program.empty()) {
            result.problems.push_back("[" + section + "]: needs a label and a program");
        } else if (std::any_of(result.games.begin(), result.games.end(),
                               [&game](const GameProfile& g) { return g.program == game.program; })) {
            result.problems.push_back("[" + section + "]: " + game.program + " is there twice");
        } else {
            result.games.push_back(std::move(game));
        }
    }
    return result;
}

std::string formatUserGames(const std::span<const GameProfile> games) {
    std::string text = "; WheelBri: the games you added in the Games tab (Add game). Written by the app.\n";
    for (const GameProfile& game : games) {
        text += "\n[game." + game.id + "]\nlabel = " + iniSafeValue(game.label) + "\nprogram = " +
                iniSafeValue(game.program) + "\n";
    }
    return text;
}

std::string gameIdFor(const std::string_view program) {
    std::string name = programName(program);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
    std::string id;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') id += c;
    }
    return id;
}

}  // namespace wheelbri
