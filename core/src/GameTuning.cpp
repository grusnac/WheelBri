#include "wheelbri/GameTuning.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/IniDocument.hpp"

#include <algorithm>
#include <charconv>

namespace wheelbri {

namespace {

// A game's own force feedback values, until 2026-11-02.
constexpr std::array<std::string_view, 12> kRetiredKeys{
    "ffb_strength", "ffb_constant", "ffb_periodic", "ffb_spring", "ffb_damper",  "ffb_friction",
    "ffb_inertia",  "ffb_cap",      "ffb_slew",     "ffb_fade_s", "ffb_invert",  "ffb_effects"};

std::optional<bool> parseBool(const std::string_view text) {
    if (text == "true" || text == "on" || text == "yes" || text == "1") return true;
    if (text == "false" || text == "off" || text == "no" || text == "0") return false;
    return std::nullopt;
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
    // Retired (until 2026-11-02): older configs load, the values are ignored.
    if (std::find(kRetiredKeys.begin(), kRetiredKeys.end(), key) != kRetiredKeys.end()) return {};
    if (key.substr(0, 7) == "button_") {
        // "button_4" (its source) or "button_4_or" (its secondary sources).
        constexpr std::string_view kOr = "_or";
        const bool secondary = key.size() > kOr.size() && key.substr(key.size() - kOr.size()) == kOr;
        const std::string_view digits = key.substr(7, key.size() - 7 - (secondary ? kOr.size() : 0));
        int target = -1;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), target);
        if (ec != std::errc() || end != digits.data() + digits.size() || target < 0 ||
            static_cast<std::size_t>(target) >= maxButtons) {
            return "no such button on the target (0 to " + std::to_string(maxButtons == 0 ? 0 : maxButtons - 1) + ")";
        }
        if (secondary) {
            const std::optional<std::vector<ButtonRef>> refs = parseButtonRefs(value);
            const bool buttonTooHigh = refs && std::any_of(refs->begin(), refs->end(), [](const ButtonRef& ref) {
                return !ref.axis && ref.source.index >= static_cast<int>(MapperConfig::kMaxSourceButtons);
            });
            if (!refs || buttonTooHigh) {
                return "expected buttons and axes, comma-separated: 20, 044f:b696/2, axis 3 > 50%, or -1 (none)";
            }
            if (refs->size() > MapperConfig::kMaxSecondaries) {
                return "at most " + std::to_string(MapperConfig::kMaxSecondaries) + " secondary sources per button";
            }
            tuning.secondaries[target] = *refs;
            return {};
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
    for (const auto& [target, source] : tuning.buttons) {
        lines.emplace_back("button_" + std::to_string(target), formatSource(source));
    }
    for (const auto& [target, refs] : tuning.secondaries) {
        lines.emplace_back("button_" + std::to_string(target) + "_or", formatButtonRefs(refs));
    }
    return lines;
}

MapperConfig tunedMapper(const MapperConfig& base, const GameTuning& tuning, std::vector<std::string>* problems) {
    MapperConfig out = base;
    PedalConfig* const pedals[3] = {&out.throttle, &out.brake, &out.clutch};
    for (std::size_t p = 0; p < 3; ++p) {
        if (tuning.pedalInvert[p]) pedals[p]->invert = *tuning.pedalInvert[p];
    }
    // The device number of a source's device in the mapping (0: the wheel), or -1 when the mapping does not read it.
    const auto deviceOf = [&out](const SourceRef& source) {
        if (source.device.empty()) return 0;
        for (std::size_t a = 0; a < out.accessoryCount(); ++a) {
            if (out.accessories[a] == source.device) return static_cast<int>(a + 1);
        }
        return -1;
    };
    const auto unread = [problems](const std::string& what) {
        if (problems != nullptr) {
            problems->push_back(what + ": that device is not in the mapping, so it is not read; the button stays as "
                                       "mapped");
        }
    };
    for (const auto& [target, source] : tuning.buttons) {
        if (target < 0 || static_cast<std::size_t>(target) >= MapperConfig::kMaxTargetButtons) continue;
        const int device = deviceOf(source);
        if (device < 0) {
            unread("button " + std::to_string(target) + " <- " + formatSource(source));
            continue;
        }
        const auto t = static_cast<std::size_t>(target);
        out.buttonMap[t] = source.mapped() ? source.index : kUnmapped;
        out.buttonDevice[t] = source.mapped() ? device : 0;
    }
    // Secondaries: the game's list instead of the mapping's, all or nothing.
    for (const auto& [target, refs] : tuning.secondaries) {
        if (target < 0 || static_cast<std::size_t>(target) >= MapperConfig::kMaxTargetButtons) continue;
        std::array<ButtonSource, MapperConfig::kMaxSecondaries> list{};
        std::size_t n = 0;
        bool read = true;
        for (const ButtonRef& ref : refs) {
            if (!ref.mapped() || n >= list.size()) continue;
            const int device = deviceOf(ref.source);
            if (device < 0) {
                unread("button " + std::to_string(target) + " or " + formatButtonRef(ref));
                read = false;
                break;
            }
            list[n++] = ButtonSource{device, ref.source.index, ref.axis, ref.invert, ref.threshold};
        }
        if (read) out.buttonSecondary[static_cast<std::size_t>(target)] = list;
    }
    return out;
}

bool changesMapping(const GameTuning& tuning) {
    for (std::size_t p = 0; p < 3; ++p) {
        if (tuning.pedalInvert[p]) return true;
    }
    return !tuning.buttons.empty() || !tuning.secondaries.empty();
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
