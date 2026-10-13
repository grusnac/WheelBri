#include "wheelbri/FileText.hpp"
#include "wheelbri/Rig.hpp"

#include "wheelbri/DeviceMatch.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <span>
#include <sstream>

namespace wheelbri {

namespace {

// The roles a control could have before 2026-10-13 ("Gas | throttle"), as written then, and the name each gave a control
// when picked: read only to keep an older rig's controls named (a role is no longer kept).
constexpr std::array<std::pair<std::string_view, std::string_view>, 32> kOlderRoles = {{
    {"steering", "Steering"},  {"throttle", "Throttle"},   {"brake", "Brake"},           {"clutch", "Clutch"},
    {"handbrake", "Handbrake"}, {"axis", "Axis"},          {"button", "Button"},         {"d-pad up", "D-pad Up"},
    {"d-pad right", "D-pad Right"}, {"d-pad down", "D-pad Down"}, {"d-pad left", "D-pad Left"},
    {"shift up", "Shift up"},  {"shift down", "Shift down"}, {"gear 1", "Gear 1"},        {"gear 2", "Gear 2"},
    {"gear 3", "Gear 3"},      {"gear 4", "Gear 4"},       {"gear 5", "Gear 5"},         {"gear 6", "Gear 6"},
    {"gear 7", "Gear 7"},      {"reverse", "Reverse"},     {"left paddle", "L Paddle"},  {"right paddle", "R Paddle"},
    {"a", "A"},                {"b", "B"},                 {"x", "X"},                   {"y", "Y"},
    {"menu", "Menu"},          {"view", "View"},           {"rsb", "RSB"},               {"lsb", "LSB"},
    {"xbox", "Xbox"},
}};

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}

// "Gas | invert" -> the control; problems go to `error`. `steering` is set when an older rig gave it the steering
// role (its device is then the wheel). An older rig's role ("Gas | throttle") and "deadzone 3%" / "full 90%" on an axis
// are skipped: a control has only a name since 2026-10-13 (a role-only control gets the role's name).
RigControl parseControl(const std::string& value, const bool axis, std::string& error, bool& steering) {
    RigControl control;
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = value.find('|', start);
        parts.push_back(trim(std::string_view(value).substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    control.label = parts[0];
    if (control.label.size() > DeviceLabels::kMaxLabel) {
        error = "at most " + std::to_string(DeviceLabels::kMaxLabel) + " characters in a name";
        control.label = truncateUtf8(std::move(control.label), DeviceLabels::kMaxLabel);
    }
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string part = lower(parts[i]);
        if (part == "invert") {
            control.invert = true;
            continue;
        }
        if (axis && (part.rfind("deadzone ", 0) == 0 || part.rfind("full ", 0) == 0)) continue;   // older: ignored
        const auto older = std::find_if(kOlderRoles.begin(), kOlderRoles.end(),
                                        [&part](const auto& role) { return role.first == part; });
        if (older != kOlderRoles.end()) {
            if (control.label.empty()) control.label = std::string(older->second);
            if (axis && part == "steering") steering = true;
        } else if (!parts[i].empty()) {
            error = "'" + parts[i] + "' is not 'invert'";
        }
    }
    return control;
}

std::string controlText(const RigControl& control) {
    std::string text = control.label;
    if (control.invert) text += " | invert";
    return text;
}

std::optional<std::string> readFile(const std::filesystem::path& path) { return readFileText(path); }

}  // namespace

const RigDevice* Rig::find(const DeviceId& id) const {
    for (const RigDevice& device : devices) {
        if (device.id == id) return &device;
    }
    return nullptr;
}

RigDevice* Rig::find(const DeviceId& id) {
    for (RigDevice& device : devices) {
        if (device.id == id) return &device;
    }
    return nullptr;
}

const RigDevice* Rig::wheel() const {
    for (const RigDevice& device : devices) {
        if (device.kind == DeviceKind::Wheel) return &device;
    }
    return devices.empty() ? nullptr : &devices.front();
}

RigResult loadRig(const IniDocument& ini) {
    RigResult result;
    for (const auto& [section, keys] : ini.sections()) {
        if (section.rfind("device_", 0) != 0) {
            result.errors.push_back("unknown section [" + section + "]");
            continue;
        }
        RigDevice device;
        bool steering = false;   // an older rig's steering role: this device is the wheel
        for (const auto& [key, value] : keys) {
            std::string error;
            if (key == "name") {
                device.name = value;
            } else if (key == "id") {
                if (const auto id = parseDeviceId(value)) {
                    device.id = *id;
                } else {
                    error = "expected vvvv:pppp (hex)";
                }
            } else if (key == "kind") {
                if (const auto kind = parseDeviceKind(value)) {
                    device.kind = *kind;
                } else {
                    error = "expected wheel, pedals, shifter, handbrake, button box or other";
                }
            } else if (key.rfind("axis_", 0) == 0 || key.rfind("button_", 0) == 0) {
                const bool axis = key[0] == 'a';
                const std::string number = key.substr(axis ? 5 : 7);
                const std::size_t count = axis ? device.axes.size() : device.buttons.size();
                std::size_t index = count;
                if (!number.empty() && std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
                    number.size() < 4) {
                    index = static_cast<std::size_t>(std::stoi(number));
                }
                if (index >= count) {
                    error = "no such " + std::string(axis ? "axis" : "button") + " (0.." + std::to_string(count - 1) + ")";
                } else {
                    RigControl& control = axis ? device.axes[index] : device.buttons[index];
                    control = parseControl(value, axis, error, steering);
                }
            } else {
                error = "unknown key";
            }
            if (!error.empty()) result.errors.push_back("[" + section + "] " + key + " = '" + value + "': " + error);
        }
        if (steering) device.kind = DeviceKind::Wheel;
        if (device.id.empty()) {
            result.errors.push_back("[" + section + "] has no id: left out");
            continue;
        }
        if (result.rig.find(device.id) != nullptr) {
            result.errors.push_back("[" + section + "] " + deviceId(device.id.vendorId, device.id.productId) +
                                    " is in the rig twice: left out");
            continue;
        }
        if (result.rig.devices.size() == Rig::kMaxDevices) {
            result.errors.push_back("[" + section + "]: at most " + std::to_string(Rig::kMaxDevices) +
                                    " devices in a rig: left out");
            continue;
        }
        result.rig.devices.push_back(std::move(device));
    }
    return result;
}

const RigDevice* rigDeviceFor(const MapperConfig& mapper, const Rig& rig, const std::string& sourceDeviceMatch,
                              const int number) {
    if (number == 0) {
        if (sourceDeviceMatch.empty()) return rig.wheel();
        for (const RigDevice& device : rig.devices) {
            if (deviceMatches(device.name, device.id.vendorId, device.id.productId, sourceDeviceMatch)) return &device;
        }
        return nullptr;
    }
    if (number < 0 || static_cast<std::size_t>(number) > mapper.accessories.size()) return nullptr;
    const DeviceId& id = mapper.accessories[static_cast<std::size_t>(number - 1)];
    return id.empty() ? nullptr : rig.find(id);
}

std::string writeRig(const Rig& rig, const std::string& name) {
    const std::string nl = "\r\n";
    std::string text = "; WheelBri rig '" + name + "': the devices and what their controls are (the Rig tab" + nl +
                       "; writes this). A control: name, or name | invert (released at the top of the axis)." + nl +
                       "; No dead zones: set those in the device's own software." + nl;
    for (std::size_t i = 0; i < rig.devices.size(); ++i) {
        const RigDevice& device = rig.devices[i];
        text += nl + "[device_" + std::to_string(i + 1) + "]" + nl;
        text += "name = " + iniSafeValue(device.name) + nl;   // from Windows or evdev: may hold ';' or '#'
        text += "id = " + deviceId(device.id.vendorId, device.id.productId) + nl;
        text += std::string("kind = ") + deviceKindName(device.kind) + nl;
        for (std::size_t a = 0; a < device.axes.size(); ++a) {
            if (device.axes[a].defined() || device.axes[a].invert) {
                text += "axis_" + std::to_string(a) + " = " + controlText(device.axes[a]) + nl;
            }
        }
        for (std::size_t b = 0; b < device.buttons.size(); ++b) {
            if (device.buttons[b].defined()) text += "button_" + std::to_string(b) + " = " + controlText(device.buttons[b]) + nl;
        }
    }
    return text;
}

DeviceLabels labelsOf(const RigDevice& device) {
    DeviceLabels labels;
    labels.name = device.name;
    labels.vendorId = device.id.vendorId;
    labels.productId = device.id.productId;
    labels.kind = device.kind;
    for (std::size_t i = 0; i < device.axes.size(); ++i) labels.axes[i] = device.axes[i].label;
    for (std::size_t i = 0; i < device.buttons.size(); ++i) labels.buttons[i] = device.buttons[i].label;
    return labels;
}

RigDevice rigDeviceFrom(const DeviceLabels& labels) {
    RigDevice device;
    device.name = labels.name;
    device.id = {labels.vendorId, labels.productId};
    device.kind = labels.kind;
    for (std::size_t i = 0; i < device.axes.size(); ++i) device.axes[i].label = labels.axes[i];
    for (std::size_t i = 0; i < device.buttons.size(); ++i) device.buttons[i].label = labels.buttons[i];
    return device;
}

std::vector<std::string> listRigs(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    forEachEntry(folder, [&names](const std::filesystem::directory_entry& entry) {
        std::error_code fileEc;
        if (entry.is_regular_file(fileEc) && lower(pathToUtf8(entry.path().extension())) == ".ini") {
            const std::string name = pathToUtf8(entry.path().stem());
            if (!name.empty()) names.push_back(name);
        }
    });
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return names;
}

std::optional<Rig> readRig(const std::filesystem::path& folder, const std::string& name,
                           std::vector<std::string>& errors) {
    const auto text = readFile(folder / (name + ".ini"));
    if (!text) return std::nullopt;
    const auto parsed = IniDocument::parse(*text);
    RigResult result = loadRig(parsed.document);
    errors = parsed.errors;
    errors.insert(errors.end(), result.errors.begin(), result.errors.end());
    return std::move(result.rig);
}

bool saveRig(const std::filesystem::path& folder, const std::string& name, const Rig& rig) {
    return writeFileReplacing(folder / (name + ".ini"), writeRig(rig, name));
}

}  // namespace wheelbri
