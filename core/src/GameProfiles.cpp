#include "wheelbri/GameProfiles.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/IniDocument.hpp"

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
                duplicate = duplicate || other.program == parsed.profile->program;
            }
            if (duplicate) {
                result.problems.push_back(std::string(file.id) + ".ini: program " + parsed.profile->program +
                                          " already has a profile: this one is not used");
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

    for (const auto& [section, keys] : ini.document.sections()) {
        if (section == "game") {
            for (const auto& [key, value] : keys) {
                if (key == "label") {
                    profile.label = value;
                } else if (key == "program") {
                    profile.program = lower(value);
                } else if (key == "target") {
                    profile.target = lower(value);
                } else {
                    result.problems.push_back("[game] " + key + ": unknown key");
                }
            }
        } else if (section == "pedals") {
            for (const auto& [key, value] : keys) {
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
                } else {
                    result.problems.push_back("[steering] " + key + ": unknown key");
                }
            }
        } else {
            result.problems.push_back("[" + section + "]: unknown section (only [game], [pedals] and [steering])");
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
    if (profile.target.empty()) result.problems.push_back("[game] target is missing (e.g. g920)");
    if (profile.rotationDeg.has_value() != !profile.rotationNote.empty()) {
        result.problems.push_back("[steering] rotation_deg and note go together: the advice and why");
    } else if (profile.rotationNote.size() > kMaxNote) {
        result.problems.push_back("[steering] note: at most " + std::to_string(kMaxNote) + " characters");
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

const GameProfile* gameProfileFor(const std::string_view nameOrPath, const std::span<const GameProfile> profiles) {
    const std::string name = programName(nameOrPath);
    if (name.empty()) return nullptr;
    for (const GameProfile& profile : profiles) {
        if (name == profile.program) return &profile;
    }
    return nullptr;
}

bool profileFitsTarget(const GameProfile& profile, const std::string_view targetProfile) {
    return profile.target == lower(targetProfile);
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
    std::vector<RunningGame> found;
    for (const GameProfile& profile : profiles) {
        for (const RunningProgram& program : programs) {
            if (gameProfileFor(program.name, profiles) == &profile) {
                found.push_back(RunningGame{&profile, program.pid, program.name});
                break;
            }
        }
    }
    return found;
}

}  // namespace wheelbri
