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
    }
    const HatButtons& hat = mapper.hatButtons;
    model.hat = {sourceOf(mapper, hat.devices[0], hat.up), sourceOf(mapper, hat.devices[1], hat.right),
                 sourceOf(mapper, hat.devices[2], hat.down), sourceOf(mapper, hat.devices[3], hat.left)};
    model.wheelRotationDeg = mapper.steering.sourceRotationDeg;
    model.gameRotationDeg = mapper.steering.targetRotationDeg;
    model.gameRotations = config.gameRotations;
    return model;
}

AutoMapResult autoMap(MappingModel& model, const Rig& rig, const IDeviceProfile& profile) {
    AutoMapResult result;
    const RigDevice* const wheel = rig.wheel();
    const auto everySource = [&model](const auto& change) {
        for (SourceRef& s : model.axes) change(s);
        for (SourceRef& s : model.buttons) change(s);
        for (SourceRef& s : model.hat) change(s);
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

    // The first control with a role takes it (devices in the rig's order, then by number); a second one is said.
    std::array<std::string, 4> axisBy;
    std::array<std::string, 4> hatBy;
    std::array<std::string, MapperConfig::kMaxTargetButtons> buttonBy;
    std::vector<SourceRef> buttonSources;   // the buttons placed now (a button drives one G920 input only)
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
                                               profile.identity().shortName + " has no handbrake axis, not mapped");
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
            axisBy[slot] = who(device, true, a);
            model.axes[slot] = source(device, a);
            model.invert[slot] = control.invert;
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
            const std::string name = g920ButtonFor(role);   // A, Menu, R paddle, Gear 1 ...: that G920 button
            std::string* by = nullptr;
            SourceRef* slot = nullptr;
            if (hat >= 0) {
                by = &hatBy[static_cast<std::size_t>(hat)];
                slot = &model.hat[static_cast<std::size_t>(hat)];
            } else if (!name.empty()) {
                const int index = target(name);
                if (index < 0) {
                    result.notes.push_back(std::string(roleName(role)) + " " + who(device, false, b) + ": the " +
                                           profile.identity().shortName + " has no " + name + ", not mapped");
                    continue;
                }
                by = &buttonBy[static_cast<std::size_t>(index)];
                slot = &model.buttons[static_cast<std::size_t>(index)];
            } else {
                continue;   // a plain button (or none): the user maps it
            }
            if (!by->empty()) {
                result.notes.push_back(std::string(roleName(role)) + " is also " + who(device, false, b) + ": " + *by +
                                       " kept");
                continue;
            }
            *by = who(device, false, b);
            *slot = source(device, b);
            buttonSources.push_back(*slot);
            ++result.set;
        }
    }

    // A button placed now drives nothing else: an older mapping of it (the default's button N <- button N) would
    // press two G920 inputs at once.
    const auto placedNow = [&buttonSources](const SourceRef& s) {
        return std::find(buttonSources.begin(), buttonSources.end(), s) != buttonSources.end();
    };
    for (std::size_t t = 0; t < model.buttons.size(); ++t) {
        if (buttonBy[t].empty() && model.buttons[t].mapped() && placedNow(model.buttons[t])) {
            result.notes.push_back(std::string(profile.buttonName(t).empty() ? "button " + std::to_string(t)
                                                                             : std::string(profile.buttonName(t))) +
                                   ": no longer " + formatSource(model.buttons[t]) + " (it has a role now)");
            model.buttons[t] = {};
        }
    }
    static constexpr std::array<const char*, 4> kHatNames = {"d-pad up", "d-pad right", "d-pad down", "d-pad left"};
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
