#include "wheelbri/MappingEdit.hpp"

#include "wheelbri/AxisTuning.hpp"
#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace wheelbri {

namespace {

std::optional<std::uint16_t> parseHex(const std::string_view text) {
    if (text.empty() || text.size() > 4) return std::nullopt;
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

std::optional<int> parseIndex(const std::string_view text) {
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

constexpr std::array<const char*, 4> kAxisSections = {"steering", "throttle", "brake", "clutch"};

bool same(const float a, const float b) { return std::fabs(a - b) < 1e-5F; }

std::string idText(const DeviceId& device) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04x:%04x", device.vendorId, device.productId);
    return text;
}

}  // namespace

std::optional<DeviceId> parseDeviceId(const std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const auto vendor = parseHex(text.substr(0, colon));
    const auto product = parseHex(text.substr(colon + 1));
    if (!vendor || !product) return std::nullopt;
    const DeviceId id{*vendor, *product};
    if (id.empty()) return std::nullopt;
    return id;
}

std::optional<SourceRef> parseSource(const std::string_view text) {
    const std::size_t slash = text.find('/');
    if (slash == std::string_view::npos) {
        const auto index = parseIndex(text);
        if (!index || *index < kUnmapped) return std::nullopt;
        return SourceRef{{}, *index};
    }
    const auto device = parseDeviceId(text.substr(0, slash));
    const auto index = parseIndex(text.substr(slash + 1));
    if (!device || !index || *index < 0) return std::nullopt;
    return SourceRef{*device, *index};
}

std::optional<ButtonRef> parseButtonRef(const std::string_view text) {
    std::string_view rest = text;
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    while (!rest.empty() && rest.back() == ' ') rest.remove_suffix(1);
    const std::size_t axisAt = rest.find("axis");
    if (axisAt == std::string_view::npos) {
        const auto source = parseSource(rest);
        if (!source) return std::nullopt;
        return ButtonRef{*source, false, false, 0.5F};
    }
    // "[vvvv:pppp/]axis N[ inverted][ > P%]"
    ButtonRef ref;
    ref.axis = true;
    if (axisAt > 0) {
        const std::string_view prefix = rest.substr(0, axisAt);
        if (prefix.back() != '/') return std::nullopt;
        const auto device = parseDeviceId(prefix.substr(0, prefix.size() - 1));
        if (!device) return std::nullopt;
        ref.source.device = *device;
    }
    std::string_view tail = rest.substr(axisAt + 4);
    if (const std::size_t gt = tail.find('>'); gt != std::string_view::npos) {
        const auto percent = tuning::parseNumber(tail.substr(gt + 1));
        if (!percent) return std::nullopt;
        ref.threshold = *percent / 100.0F;
        if (ref.threshold < kMinButtonThreshold - 1e-4F || ref.threshold > kMaxButtonThreshold + 1e-4F) return std::nullopt;
        tail = tail.substr(0, gt);
    }
    if (const std::size_t inv = tail.find("inverted"); inv != std::string_view::npos) {
        ref.invert = true;
        tail = tail.substr(0, inv);
    }
    while (!tail.empty() && tail.front() == ' ') tail.remove_prefix(1);
    while (!tail.empty() && tail.back() == ' ') tail.remove_suffix(1);
    const auto index = parseIndex(tail);
    if (!index || *index < 0 || static_cast<std::size_t>(*index) >= RawInputState::kMaxAxes) return std::nullopt;
    ref.source.index = *index;
    return ref;
}

std::string formatButtonRef(const ButtonRef& ref) {
    if (!ref.mapped()) return std::to_string(kUnmapped);
    if (!ref.axis) return formatSource(ref.source);
    std::string text = (ref.source.device.empty() ? std::string() : idText(ref.source.device) + "/") + "axis " +
                       std::to_string(ref.source.index);
    if (ref.invert) text += " inverted";
    return text + " > " + tuning::formatPercent(ref.threshold);
}

std::optional<std::vector<ButtonRef>> parseButtonRefs(const std::string_view text) {
    std::vector<ButtonRef> refs;
    std::string_view rest = text;
    bool none = false;
    while (true) {
        const std::size_t comma = rest.find(',');
        std::string_view item = rest.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item.empty()) {
            if (comma != std::string_view::npos || !refs.empty()) return std::nullopt;   // "1,,2", "1,"
        } else {
            const std::optional<ButtonRef> ref = parseButtonRef(item);
            if (!ref) return std::nullopt;
            if (ref->mapped()) {
                refs.push_back(*ref);
            } else {
                none = true;
            }
        }
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
    }
    if (none && !refs.empty()) return std::nullopt;   // "-1, 4": one or the other
    return refs;
}

std::string formatButtonRefs(const std::vector<ButtonRef>& refs) {
    std::string text;
    for (const ButtonRef& ref : refs) {
        if (!ref.mapped()) continue;
        if (!text.empty()) text += ", ";
        text += formatButtonRef(ref);
    }
    return text.empty() ? std::to_string(kUnmapped) : text;
}

std::string formatSource(const SourceRef& source) {
    if (!source.mapped()) return std::to_string(kUnmapped);
    if (source.device.empty()) return std::to_string(source.index);
    return idText(source.device) + "/" + std::to_string(source.index);
}

std::optional<int> deviceNumber(MapperConfig& mapper, const DeviceId& device) {
    if (device.empty()) return 0;
    for (std::size_t i = 0; i < mapper.accessories.size(); ++i) {
        if (mapper.accessories[i] == device) return static_cast<int>(i + 1);
        if (mapper.accessories[i].empty()) {
            mapper.accessories[i] = device;
            return static_cast<int>(i + 1);
        }
    }
    return std::nullopt;
}

SourceRef sourceOf(const MapperConfig& mapper, const int device, const int index) {
    if (index < 0) return {};
    if (device == 0) return {{}, index};
    if (device < 0 || static_cast<std::size_t>(device) > mapper.accessories.size()) return {};
    const DeviceId& id = mapper.accessories[static_cast<std::size_t>(device - 1)];
    if (id.empty()) return {};
    return {id, index};
}

MappingModel mappingModel(const EmulatorConfig& config) {
    const MapperConfig& mapper = config.mapper;
    MappingModel model;
    model.sourceDevice = config.sourceDeviceMatch;
    model.axes = {sourceOf(mapper, mapper.steering.sourceDevice, mapper.steering.sourceAxis),
                  sourceOf(mapper, mapper.throttle.sourceDevice, mapper.throttle.sourceAxis),
                  sourceOf(mapper, mapper.brake.sourceDevice, mapper.brake.sourceAxis),
                  sourceOf(mapper, mapper.clutch.sourceDevice, mapper.clutch.sourceAxis)};
    model.invert = {mapper.steering.invert, mapper.throttle.invert, mapper.brake.invert, mapper.clutch.invert};
    for (std::size_t target = 0; target < model.buttons.size(); ++target) {
        model.buttons[target] = sourceOf(mapper, mapper.buttonDevice[target], mapper.buttonMap[target]);
        for (const ButtonSource& alt : mapper.buttonSecondary[target]) {
            const ButtonRef ref{sourceOf(mapper, alt.device, alt.index), alt.axis, alt.invert, alt.threshold};
            if (ref.mapped()) model.secondaries[target].push_back(ref);
        }
    }
    const HatButtons& hat = mapper.hatButtons;
    model.hat = {sourceOf(mapper, hat.devices[0], hat.up), sourceOf(mapper, hat.devices[1], hat.right),
                 sourceOf(mapper, hat.devices[2], hat.down), sourceOf(mapper, hat.devices[3], hat.left)};
    model.wheelRotationDeg = mapper.steering.sourceRotationDeg;
    model.gameRotationDeg = mapper.steering.targetRotationDeg;
    model.gameRotations = config.gameRotations;
    return model;
}

namespace {

constexpr std::array<const char*, 4> kAxisUseNames = {"steering", "throttle", "brake", "clutch"};
constexpr std::array<const char*, 4> kHatUseNames = {"D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left"};

// A source as the mapping compares them: on the wheel's own id it is the wheel's.
SourceRef normalSource(const MappingModel& model, SourceRef source) {
    const std::optional<DeviceId> wheel = parseDeviceId(model.sourceDevice);
    if (wheel && source.device == *wheel) source.device = {};
    return source;
}

// Calls use(control, how) for every source the mapping uses (the target's named buttons only).
template <typename Use>
void forEachUse(const MappingModel& model, const IDeviceProfile& profile, const Use& use) {
    for (std::size_t i = 0; i < model.axes.size(); ++i) {
        if (model.axes[i].mapped()) use(ControlRef{model.axes[i], true}, ControlUse{ControlUse::Kind::Axis, i});
    }
    for (std::size_t i = 0; i < model.hat.size(); ++i) {
        if (model.hat[i].mapped()) use(ControlRef{model.hat[i], false}, ControlUse{ControlUse::Kind::Hat, i});
    }
    for (std::size_t i = 0; i < model.buttons.size(); ++i) {
        if (model.buttons[i].mapped() && !profile.buttonName(i).empty()) {
            use(ControlRef{model.buttons[i], false}, ControlUse{ControlUse::Kind::Button, i});
        }
    }
    for (std::size_t i = 0; i < model.secondaries.size(); ++i) {
        if (profile.buttonName(i).empty()) continue;
        for (const ButtonRef& ref : model.secondaries[i]) {
            if (ref.mapped()) use(ControlRef{ref.source, ref.axis}, ControlUse{ControlUse::Kind::Secondary, i});
        }
    }
}

std::string controlText(const ControlRef& control) {
    return std::string(control.axis ? "axis " : "button ") + std::to_string(control.source.index) + " of " +
           (control.source.device.empty() ? std::string("the wheel") : idText(control.source.device));
}

}  // namespace

std::vector<ControlUse> usesOf(const MappingModel& model, const IDeviceProfile& profile, const ControlRef& control) {
    std::vector<ControlUse> uses;
    const ControlRef wanted{normalSource(model, control.source), control.axis};
    forEachUse(model, profile, [&](const ControlRef& used, const ControlUse& use) {
        if (ControlRef{normalSource(model, used.source), used.axis} == wanted) uses.push_back(use);
    });
    return uses;
}

std::string describeUse(const ControlUse& use, const IDeviceProfile& profile) {
    const std::string target = profile.identity().shortName;
    switch (use.kind) {
        case ControlUse::Kind::Axis:
            return "Role: " + target + " " + (use.index < kAxisUseNames.size() ? kAxisUseNames[use.index] : "axis");
        case ControlUse::Kind::Hat:
            return "Role: " + target + " " + (use.index < kHatUseNames.size() ? kHatUseNames[use.index] : "D-pad");
        case ControlUse::Kind::Button: return "Role: " + target + " " + std::string(profile.buttonName(use.index));
        case ControlUse::Kind::Secondary:
            return "Secondary of " + target + " " + std::string(profile.buttonName(use.index));
    }
    return {};
}

std::vector<std::string> controlsUsedTwice(const MappingModel& model, const IDeviceProfile& profile) {
    std::vector<std::pair<ControlRef, std::vector<ControlUse>>> all;
    forEachUse(model, profile, [&](const ControlRef& used, const ControlUse& use) {
        const ControlRef key{normalSource(model, used.source), used.axis};
        const auto it = std::find_if(all.begin(), all.end(), [&key](const auto& entry) { return entry.first == key; });
        if (it == all.end()) {
            all.push_back({key, {use}});
        } else {
            it->second.push_back(use);
        }
    });
    std::vector<std::string> lines;
    for (const auto& [control, uses] : all) {
        if (uses.size() < 2) continue;
        std::string line = controlText(control) + ": ";
        for (std::size_t i = 0; i < uses.size(); ++i) {
            std::string what = describeUse(uses[i], profile);
            what[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(what[0])));
            line += (i > 0 ? ", " : "") + what;
        }
        lines.push_back(line);
    }
    return lines;
}

AutoMapResult autoMap(MappingModel& model, const Rig& rig, const IDeviceProfile& profile) {
    AutoMapResult result;
    const RigDevice* const wheel = rig.wheel();
    const auto everySource = [&model](const auto& change) {
        for (SourceRef& s : model.axes) change(s);
        for (SourceRef& s : model.buttons) change(s);
        for (SourceRef& s : model.hat) change(s);
        for (auto& list : model.secondaries) {
            for (ButtonRef& b : list) change(b.source);
        }
    };
    if (wheel != nullptr) {
        // Another wheel than the mapping's: what read the old one keeps reading it (now as an accessory, as the
        // Mapping tab's Change does); what names the rig's wheel by id becomes the wheel's.
        const std::optional<DeviceId> old = parseDeviceId(model.sourceDevice);
        if (old && *old != wheel->id) {
            everySource([&old](SourceRef& s) {
                if (s.mapped() && s.device.empty()) s.device = *old;
            });
        }
        everySource([wheel](SourceRef& s) {
            if (s.device == wheel->id) s.device = {};
        });
        model.sourceDevice = idText(wheel->id);
    }
    const auto source = [wheel](const RigDevice& device, const std::size_t index) {
        return SourceRef{wheel != nullptr && device.id == wheel->id ? DeviceId{} : device.id, static_cast<int>(index)};
    };
    // "'Gas' (axis 2 of Pedals)", for the notes.
    const auto who = [](const RigDevice& device, const bool axis, const std::size_t index) {
        const RigControl& control = axis ? device.axes[index] : device.buttons[index];
        const std::string name = device.name.empty() ? idText(device.id) : device.name;
        return (control.label.empty() ? std::string() : "'" + control.label + "' ") + "(" + (axis ? "axis " : "button ") +
               std::to_string(index) + " of " + name + ")";
    };
    const auto target = [&profile](const std::string& name) -> int {
        for (std::size_t i = 0; i < MapperConfig::kMaxTargetButtons; ++i) {
            if (profile.buttonName(i) == name) return static_cast<int>(i);
        }
        return -1;
    };

    // The secondaries are the user's: a control that is one stays one (and is placed nowhere by its role).
    const auto secondaryOf = [&model, &profile](const SourceRef& s, const bool axis) -> std::string {
        for (std::size_t t = 0; t < model.secondaries.size(); ++t) {
            for (const ButtonRef& ref : model.secondaries[t]) {
                if (ref.axis == axis && normalSource(model, ref.source) == s) {
                    return profile.buttonName(t).empty() ? "button " + std::to_string(t)
                                                         : std::string(profile.buttonName(t));
                }
            }
        }
        return {};
    };
    const auto keptSecondary = [&](const RigDevice& device, const bool axis, const std::size_t index,
                                   const std::string& place, const std::string& of) {
        result.notes.push_back(who(device, axis, index) + " is a secondary of " + of + ": " + place +
                               " left empty; remove it from " + of + " to put it on " + place);
    };

    // The first control with a role takes it (devices in the rig's order, then by number); a second one is said.
    std::array<std::string, 4> axisBy;
    std::array<std::string, 4> hatBy;
    std::array<std::string, MapperConfig::kMaxTargetButtons> buttonBy;
    std::vector<ControlRef> placed;   // the controls placed now (a control is used once)
    bool handbrakeSaid = false;
    for (const RigDevice& device : rig.devices) {
        for (std::size_t a = 0; a < device.axes.size(); ++a) {
            const RigControl& control = device.axes[a];
            std::size_t slot = model.axes.size();
            switch (control.role) {
                case ControlRole::Steering: slot = MappingModel::kSteering; break;
                case ControlRole::Throttle: slot = MappingModel::kThrottle; break;
                case ControlRole::Brake: slot = MappingModel::kBrake; break;
                case ControlRole::Clutch: slot = MappingModel::kClutch; break;
                case ControlRole::Handbrake:
                    if (!handbrakeSaid) {
                        result.notes.push_back("handbrake " + who(device, true, a) + ": the " +
                                               profile.identity().shortName + " has no handbrake axis; add it as a "
                                               "secondary of the button your game brakes with");
                        handbrakeSaid = true;
                    }
                    break;
                default: break;
            }
            if (slot == model.axes.size()) continue;
            if (!axisBy[slot].empty()) {
                result.notes.push_back(std::string(roleName(control.role)) + " is also " + who(device, true, a) +
                                       ": " + axisBy[slot] + " kept");
                continue;
            }
            if (const std::string of = secondaryOf(source(device, a), true); !of.empty()) {
                keptSecondary(device, true, a, profile.identity().shortName + " " + roleName(control.role), of);
                model.axes[slot] = {};
                continue;
            }
            axisBy[slot] = who(device, true, a);
            model.axes[slot] = source(device, a);
            model.invert[slot] = control.invert;
            placed.push_back({model.axes[slot], true});
            ++result.set;
        }
        for (std::size_t b = 0; b < device.buttons.size(); ++b) {
            const ControlRole role = device.buttons[b].role;
            int hat = -1;
            switch (role) {
                case ControlRole::DpadUp: hat = 0; break;
                case ControlRole::DpadRight: hat = 1; break;
                case ControlRole::DpadDown: hat = 2; break;
                case ControlRole::DpadLeft: hat = 3; break;
                default: break;
            }
            const std::string name = g920ButtonFor(role);   // A, Menu, R Paddle, Gear 1 ...: that G920 button
            std::string* by = nullptr;
            SourceRef* slot = nullptr;
            std::string place;
            static constexpr std::array<const char*, 4> kHatPlaces = {"D-pad Up", "D-pad Right", "D-pad Down",
                                                                      "D-pad Left"};
            if (hat >= 0) {
                by = &hatBy[static_cast<std::size_t>(hat)];
                slot = &model.hat[static_cast<std::size_t>(hat)];
                place = profile.identity().shortName + " " + kHatPlaces[static_cast<std::size_t>(hat)];
            } else if (!name.empty()) {
                const int index = target(name);
                if (index < 0) {
                    result.notes.push_back(std::string(roleName(role)) + " " + who(device, false, b) + ": the " +
                                           profile.identity().shortName + " has no " + name + ", not mapped");
                    continue;
                }
                by = &buttonBy[static_cast<std::size_t>(index)];
                slot = &model.buttons[static_cast<std::size_t>(index)];
                place = profile.identity().shortName + " " + name;
            } else {
                continue;   // a plain button (or none): the user maps it
            }
            if (!by->empty()) {
                result.notes.push_back(std::string(roleName(role)) + " is also " + who(device, false, b) + ": " + *by +
                                       " kept");
                continue;
            }
            if (const std::string of = secondaryOf(source(device, b), false); !of.empty()) {
                keptSecondary(device, false, b, place, of);
                *slot = {};
                continue;
            }
            *by = who(device, false, b);
            *slot = source(device, b);
            placed.push_back({*slot, false});
            ++result.set;
        }
    }

    // A control placed now drives nothing else: an older mapping of it (the default's button N <- button N) would
    // press two G920 inputs at once.
    const auto placedNow = [&placed, &model](const SourceRef& s, const bool axis = false) {
        return std::find(placed.begin(), placed.end(), ControlRef{normalSource(model, s), axis}) != placed.end();
    };
    for (std::size_t i = 0; i < model.axes.size(); ++i) {
        if (axisBy[i].empty() && model.axes[i].mapped() && placedNow(model.axes[i], true)) {
            result.notes.push_back(std::string(kAxisUseNames[i]) + ": no longer " + formatSource(model.axes[i]) +
                                   " (it has a role now)");
            model.axes[i] = {};
        }
    }
    for (std::size_t t = 0; t < model.buttons.size(); ++t) {
        if (buttonBy[t].empty() && model.buttons[t].mapped() && placedNow(model.buttons[t])) {
            result.notes.push_back(std::string(profile.buttonName(t).empty() ? "button " + std::to_string(t)
                                                                             : std::string(profile.buttonName(t))) +
                                   ": no longer " + formatSource(model.buttons[t]) + " (it has a role now)");
            model.buttons[t] = {};
        }
    }
    static constexpr std::array<const char*, 4> kHatNames = {"D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left"};
    for (std::size_t h = 0; h < model.hat.size(); ++h) {
        if (hatBy[h].empty() && model.hat[h].mapped() && placedNow(model.hat[h])) {
            result.notes.push_back(std::string(kHatNames[h]) + ": no longer " + formatSource(model.hat[h]) +
                                   " (it has a role now)");
            model.hat[h] = {};
        }
    }
    return result;
}

std::string tuningProblem(const MappingModel& model) {
    if (std::string problem = tuning::rotationProblem(model.wheelRotationDeg); !problem.empty()) {
        return "wheel rotation: " + problem;
    }
    if (std::string problem = tuning::rotationProblem(model.gameRotationDeg); !problem.empty()) {
        return "game rotation: " + problem;
    }
    for (std::size_t t = 0; t < model.secondaries.size(); ++t) {
        if (model.secondaries[t].size() > MapperConfig::kMaxSecondaries) {
            return "button " + std::to_string(t) + ": at most " + std::to_string(MapperConfig::kMaxSecondaries) +
                   " secondary sources";
        }
        for (const ButtonRef& alt : model.secondaries[t]) {
            if (alt.mapped() && alt.axis &&
                (alt.threshold < kMinButtonThreshold - 1e-4F || alt.threshold > kMaxButtonThreshold + 1e-4F)) {
                return "button " + std::to_string(t) + ": an axis presses it between " +
                       tuning::formatPercent(kMinButtonThreshold) + " and " +
                       tuning::formatPercent(kMaxButtonThreshold);
            }
        }
    }
    for (const auto& [game, value] : model.gameRotations) {
        if (value.off) continue;
        for (const float deg : {value.rotation.wheelDeg, value.rotation.gameDeg}) {
            if (std::string problem = tuning::rotationProblem(deg); !problem.empty()) return game + ": " + problem;
        }
    }
    return {};
}

MappingWriteResult writeMapping(const std::string_view base, const MappingModel& model) {
    MappingWriteResult result;
    result.error = tuningProblem(model);
    if (!result.error.empty()) return result;
    const std::optional<DeviceId> wheel = parseDeviceId(model.sourceDevice);
    // On the wheel's own id: the wheel's (device 0), so the same device is not opened twice.
    const auto normal = [&wheel](SourceRef source) {
        if (wheel && source.device == *wheel) source.device = {};
        return source;
    };

    std::set<std::pair<std::uint16_t, std::uint16_t>> accessories;
    const auto count = [&](const SourceRef& source) {
        const SourceRef s = normal(source);
        if (s.mapped() && !s.device.empty()) accessories.insert({s.device.vendorId, s.device.productId});
    };
    for (const SourceRef& s : model.axes) count(s);
    for (const SourceRef& s : model.buttons) count(s);
    for (const SourceRef& s : model.hat) count(s);
    for (const auto& list : model.secondaries) {
        for (const ButtonRef& b : list) count(b.source);
    }
    if (accessories.size() > MapperConfig::kMaxAccessories) {
        result.error = "the mapping uses " + std::to_string(accessories.size()) + " accessories besides the wheel; at most " +
                       std::to_string(MapperConfig::kMaxAccessories) + " can be read at once";
        return result;
    }

    const IniDocument before = IniDocument::parse(base).document;
    IniEditor ini(base);
    ini.set("emulator", "source_device", model.sourceDevice);
    // A number: left alone when the file already says it (or, with no line, when it is the default).
    const auto number = [&](const char* section, const char* key, const float value, const float defaultValue) {
        const std::optional<std::string> line = before.get(section, key);
        const std::optional<float> current = line ? tuning::parseNumber(*line) : std::optional<float>(defaultValue);
        if (current && same(*current, value)) return;
        ini.set(section, key, tuning::formatNumber(value));
    };
    // The defaults as the config reader has them (MapperConfig.hpp).
    number("steering", "source_rotation_deg", model.wheelRotationDeg, SteeringConfig{}.sourceRotationDeg);
    number("steering", "target_rotation_deg", model.gameRotationDeg, SteeringConfig{}.targetRotationDeg);
    // [game_rotation]: one line per game the user set; a line for a game no longer set goes.
    const auto& sections = before.sections();
    if (const auto old = sections.find("game_rotation"); old != sections.end()) {
        for (const auto& [game, value] : old->second) {
            if (model.gameRotations.count(game) == 0) ini.remove("game_rotation", game);
        }
    }
    for (const auto& [game, value] : model.gameRotations) {
        const std::optional<std::string> line = before.get("game_rotation", game);
        const std::optional<GameRotationOverride> current = line ? parseGameRotation(*line) : std::nullopt;
        if (!current || !(*current == value)) ini.set("game_rotation", game, formatGameRotation(value));
    }
    for (std::size_t i = 0; i < kAxisSections.size(); ++i) {
        ini.set(kAxisSections[i], "axis", formatSource(normal(model.axes[i])));
        // false is the default: written only over a line that is there already.
        if (model.invert[i] || before.get(kAxisSections[i], "invert")) {
            ini.set(kAxisSections[i], "invert", model.invert[i] ? "true" : "false");
        }
    }
    for (std::size_t target = 0; target < model.buttons.size(); ++target) {
        const SourceRef source = normal(model.buttons[target]);
        const std::string key = "target_" + std::to_string(target);
        if (source == SourceRef{{}, static_cast<int>(target)}) {
            ini.remove("buttons", key);   // the default
        } else {
            ini.set("buttons", key, formatSource(source));
        }
        std::vector<ButtonRef> secondaries;
        for (ButtonRef alt : model.secondaries[target]) {
            alt.source = normal(alt.source);
            if (alt.mapped()) secondaries.push_back(alt);
        }
        if (secondaries.empty()) {
            ini.remove("buttons", key + "_or");
        } else if (const std::optional<std::string> line = before.get("buttons", key + "_or");
                   !line || parseButtonRefs(*line) != secondaries) {
            ini.set("buttons", key + "_or", formatButtonRefs(secondaries));   // unchanged: its text stays
        }
    }
    static constexpr std::array<const char*, 4> kHatKeys = {"up", "right", "down", "left"};
    for (std::size_t i = 0; i < kHatKeys.size(); ++i) {
        const SourceRef source = normal(model.hat[i]);
        if (source.mapped()) {
            ini.set("hat", kHatKeys[i], formatSource(source));
        } else {
            ini.remove("hat", kHatKeys[i]);
        }
    }
    result.text = ini.text();
    return result;
}

}  // namespace wheelbri
