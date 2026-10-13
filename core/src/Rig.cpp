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

constexpr std::array<std::pair<ControlRole, const char*>, 32> kRoleNames = {{
    {ControlRole::Steering, "steering"},   {ControlRole::Throttle, "throttle"},   {ControlRole::Brake, "brake"},
    {ControlRole::Clutch, "clutch"},       {ControlRole::Handbrake, "handbrake"}, {ControlRole::OtherAxis, "axis"},
    {ControlRole::Button, "button"},       {ControlRole::DpadUp, "d-pad up"},     {ControlRole::DpadRight, "d-pad right"},
    {ControlRole::DpadDown, "d-pad down"}, {ControlRole::DpadLeft, "d-pad left"}, {ControlRole::ShiftUp, "shift up"},
    {ControlRole::ShiftDown, "shift down"}, {ControlRole::Gear1, "gear 1"},       {ControlRole::Gear2, "gear 2"},
    {ControlRole::Gear3, "gear 3"},        {ControlRole::Gear4, "gear 4"},        {ControlRole::Gear5, "gear 5"},
    {ControlRole::Gear6, "gear 6"},        {ControlRole::Gear7, "gear 7"},        {ControlRole::GearReverse, "reverse"},
    {ControlRole::LeftPaddle, "left paddle"}, {ControlRole::RightPaddle, "right paddle"},
    {ControlRole::ButtonA, "a"},           {ControlRole::ButtonB, "b"},           {ControlRole::ButtonX, "x"},
    {ControlRole::ButtonY, "y"},           {ControlRole::Menu, "menu"},           {ControlRole::View, "view"},
    {ControlRole::Rsb, "rsb"},             {ControlRole::Lsb, "lsb"},             {ControlRole::Xbox, "xbox"},
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

// "Gas | throttle | invert" -> the control; problems go to `error`. An older rig's "deadzone 3%" / "full 90%" on an
// axis is skipped (dead zones are the device's own software's since 2026-10-13).
RigControl parseControl(const std::string& value, const bool axis, std::string& error) {
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
        } else if (axis && (part.rfind("deadzone ", 0) == 0 || part.rfind("full ", 0) == 0)) {
            continue;   // a dead zone of an older rig: ignored
        } else if (const auto role = parseRole(parts[i]); role && roleFits(*role, axis)) {
            control.role = *role;
        } else if (!parts[i].empty()) {
            error = "'" + parts[i] + "' is not " + (axis ? "an axis role or 'invert'" : "a button role or 'invert'");
        }
    }
    // A name made by picking the role, written the way the role is named now ("D-pad up" -> "D-pad Up"; the paddles
    // were "Left paddle" / "Right paddle"); a name of the user's own stays as it is.
    if (!control.label.empty() &&
        (lower(control.label) == lower(roleLabel(control.role)) || lower(control.label) == roleName(control.role))) {
        control.label = roleLabel(control.role);
    }
    return control;
}

std::string controlText(const RigControl& control) {
    std::string text = control.label;
    if (control.role != ControlRole::None) text += std::string(" | ") + roleName(control.role);
    if (control.invert) text += " | invert";
    return text;
}

std::optional<std::string> readFile(const std::filesystem::path& path) { return readFileText(path); }

}  // namespace

const char* roleName(const ControlRole role) {
    for (const auto& [value, name] : kRoleNames) {
        if (value == role) return name;
    }
    return "";
}

std::optional<ControlRole> parseRole(const std::string_view text) {
    const std::string wanted = lower(trim(text));
    for (const auto& [value, name] : kRoleNames) {
        if (wanted == name) return value;
    }
    return std::nullopt;
}

bool isAxisRole(const ControlRole role) {
    return std::find(kAxisRoles.begin(), kAxisRoles.end(), role) != kAxisRoles.end();
}

std::string roleLabel(const ControlRole role) {
    switch (role) {
        case ControlRole::ButtonA: return "A";
        case ControlRole::ButtonB: return "B";
        case ControlRole::ButtonX: return "X";
        case ControlRole::ButtonY: return "Y";
        case ControlRole::Rsb: return "RSB";
        case ControlRole::Lsb: return "LSB";
        case ControlRole::LeftPaddle: return "L Paddle";
        case ControlRole::RightPaddle: return "R Paddle";
        case ControlRole::DpadUp: return "D-pad Up";
        case ControlRole::DpadRight: return "D-pad Right";
        case ControlRole::DpadDown: return "D-pad Down";
        case ControlRole::DpadLeft: return "D-pad Left";
        default: break;
    }
    std::string text = roleName(role);
    if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return text;
}

std::string g920ButtonFor(const ControlRole role) {
    switch (role) {
        case ControlRole::ButtonA: return "A";
        case ControlRole::ButtonB: return "B";
        case ControlRole::ButtonX: return "X";
        case ControlRole::ButtonY: return "Y";
        case ControlRole::Menu: return "Menu";
        case ControlRole::View: return "View";
        case ControlRole::Rsb: return "RSB";
        case ControlRole::Lsb: return "LSB";
        case ControlRole::Xbox: return "Xbox";
        case ControlRole::RightPaddle:   // the G920's paddles; shifting up and down is what games use them for
        case ControlRole::ShiftUp: return "R Paddle";
        case ControlRole::LeftPaddle:
        case ControlRole::ShiftDown: return "L Paddle";
        case ControlRole::GearReverse: return "Gear R";
        default: break;
    }
    if (role >= ControlRole::Gear1 && role <= ControlRole::Gear7) {
        return "Gear " + std::to_string(static_cast<int>(role) - static_cast<int>(ControlRole::Gear1) + 1);
    }
    return {};
}

bool roleFits(const ControlRole role, const bool axis) {
    if (role == ControlRole::Handbrake) return true;
    return isAxisRole(role) == axis;
}

bool axisDrivesButtons(const ControlRole role) { return isPedal(role) || role == ControlRole::Handbrake; }

bool isPedal(const ControlRole role) {
    return role == ControlRole::Throttle || role == ControlRole::Brake || role == ControlRole::Clutch;
}

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
        for (const RigControl& axis : device.axes) {
            if (axis.role == ControlRole::Steering) return &device;
        }
    }
    for (const RigDevice& device : devices) {
        if (device.kind == DeviceKind::Wheel) return &device;
    }
    return nullptr;
}

RigResult loadRig(const IniDocument& ini) {
    RigResult result;
    for (const auto& [section, keys] : ini.sections()) {
        if (section.rfind("device_", 0) != 0) {
            result.errors.push_back("unknown section [" + section + "]");
            continue;
        }
        RigDevice device;
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
                    control = parseControl(value, axis, error);
                }
            } else {
                error = "unknown key";
            }
            if (!error.empty()) result.errors.push_back("[" + section + "] " + key + " = '" + value + "': " + error);
        }
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

std::string sameRoleElsewhere(const Rig& rig, const DeviceId& device, const bool axis, const std::size_t index,
                              const ControlRole role) {
    if (role == ControlRole::None || role == ControlRole::Button || role == ControlRole::OtherAxis) return {};
    const std::string g920 = g920ButtonFor(role);
    const auto clashes = [&](const ControlRole other) {
        return other == role || (!g920.empty() && g920ButtonFor(other) == g920);
    };
    for (const RigDevice& candidate : rig.devices) {
        const std::span<const RigControl> controls =
            axis ? std::span<const RigControl>(candidate.axes) : std::span<const RigControl>(candidate.buttons);
        for (std::size_t i = 0; i < controls.size(); ++i) {
            if (candidate.id == device && i == index) continue;
            if (!clashes(controls[i].role)) continue;
            const std::string name =
                candidate.name.empty() ? deviceId(candidate.id.vendorId, candidate.id.productId) : candidate.name;
            return std::string(axis ? "axis " : "button ") + std::to_string(i) +
                   (controls[i].label.empty() ? std::string() : " ('" + controls[i].label + "')") + " of " + name;
        }
    }
    return {};
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

std::vector<std::string> buttonAxisProblems(const MapperConfig& mapper, const Rig& rig,
                                            const std::string& sourceDeviceMatch) {
    std::vector<std::string> problems;
    for (std::size_t t = 0; t < mapper.buttonSecondary.size(); ++t) {
        for (const ButtonSource& alt : mapper.buttonSecondary[t]) {
            if (!alt.mapped() || !alt.axis) continue;
            const RigDevice* const device = rigDeviceFor(mapper, rig, sourceDeviceMatch, alt.device);
            if (device == nullptr || alt.index >= static_cast<int>(device->axes.size())) continue;   // not in the rig
            const ControlRole role = device->axes[static_cast<std::size_t>(alt.index)].role;
            if (!axisDrivesButtons(role)) {
                problems.push_back("[buttons] target_" + std::to_string(t) + "_or: axis " + std::to_string(alt.index) +
                                   " of " + (device->name.empty() ? deviceId(device->id.vendorId, device->id.productId)
                                                                  : device->name) +
                                   " is " + (role == ControlRole::None ? std::string("not named in the rig")
                                                                       : std::string("the ") + roleName(role)) +
                                   ": only a throttle, brake, clutch or handbrake axis should press a button (it is "
                                   "used anyway)");
            }
        }
    }
    return problems;
}

std::string writeRig(const Rig& rig, const std::string& name) {
    const std::string nl = "\r\n";
    std::string text = "; WheelBri rig '" + name + "': the devices and what their controls are (the Rig tab" + nl +
                       "; writes this). A control: name | role | invert (invert = released at the top of the axis)." + nl +
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
