#include "wheelbri/MappingEdit.hpp"

#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"

#include <charconv>
#include <cstdio>
#include <set>

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
    return model;
}

int autoMap(MappingModel& model, const Rig& rig, const IDeviceProfile& profile) {
    const RigDevice* const wheel = rig.wheel();
    if (wheel != nullptr) model.sourceDevice = idText(wheel->id);
    const auto source = [wheel](const RigDevice& device, const std::size_t index) {
        return SourceRef{wheel != nullptr && device.id == wheel->id ? DeviceId{} : device.id, static_cast<int>(index)};
    };
    const auto target = [&profile](const std::string& name) -> int {
        for (std::size_t i = 0; i < MapperConfig::kMaxTargetButtons; ++i) {
            if (profile.buttonName(i) == name) return static_cast<int>(i);
        }
        return -1;
    };
    int set = 0;
    for (const RigDevice& device : rig.devices) {
        for (std::size_t a = 0; a < device.axes.size(); ++a) {
            const RigControl& control = device.axes[a];
            std::size_t slot = model.axes.size();
            switch (control.role) {
                case ControlRole::Steering: slot = MappingModel::kSteering; break;
                case ControlRole::Throttle: slot = MappingModel::kThrottle; break;
                case ControlRole::Brake: slot = MappingModel::kBrake; break;
                case ControlRole::Clutch: slot = MappingModel::kClutch; break;
                default: break;
            }
            if (slot == model.axes.size()) continue;
            model.axes[slot] = source(device, a);
            model.invert[slot] = control.invert;
            ++set;
        }
        for (std::size_t b = 0; b < device.buttons.size(); ++b) {
            const ControlRole role = device.buttons[b].role;
            int hat = -1;
            std::string name;
            switch (role) {
                case ControlRole::DpadUp: hat = 0; break;
                case ControlRole::DpadRight: hat = 1; break;
                case ControlRole::DpadDown: hat = 2; break;
                case ControlRole::DpadLeft: hat = 3; break;
                case ControlRole::RightPaddle:   // the G920's paddles; shifting up and down is what games use them for
                case ControlRole::ShiftUp: name = "R paddle"; break;
                case ControlRole::LeftPaddle:
                case ControlRole::ShiftDown: name = "L paddle"; break;
                case ControlRole::GearReverse: name = "Gear R"; break;
                default:
                    if (role >= ControlRole::Gear1 && role <= ControlRole::Gear7) {
                        name = "Gear " + std::to_string(static_cast<int>(role) - static_cast<int>(ControlRole::Gear1) + 1);
                    }
                    break;
            }
            if (hat >= 0) {
                model.hat[static_cast<std::size_t>(hat)] = source(device, b);
                ++set;
            } else if (const int index = name.empty() ? -1 : target(name); index >= 0) {
                model.buttons[static_cast<std::size_t>(index)] = source(device, b);
                ++set;
            }
        }
    }
    return set;
}

MappingWriteResult writeMapping(const std::string_view base, const MappingModel& model) {
    MappingWriteResult result;
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
    static constexpr std::array<const char*, 4> kAxisSections = {"steering", "throttle", "brake", "clutch"};
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
