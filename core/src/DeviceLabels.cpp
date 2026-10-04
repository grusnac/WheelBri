#include "wheelbri/FileText.hpp"
#include "wheelbri/DeviceLabels.hpp"

#include "wheelbri/IniDocument.hpp"
#include "wheelbri/IniEditor.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace wheelbri {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

template <std::size_t N>
void readLabels(const IniDocument& ini, const std::string& section, const std::string& prefix,
                std::array<std::string, N>& labels, std::vector<std::string>& errors) {
    std::set<std::string> known;
    for (std::size_t i = 0; i < N; ++i) {
        const std::string key = prefix + std::to_string(i);
        known.insert(key);
        std::string label = ini.get(section, key).value_or("");
        if (label.size() > DeviceLabels::kMaxLabel) {
            errors.push_back("[" + section + "] " + key + " = '" + label + "': at most " +
                             std::to_string(DeviceLabels::kMaxLabel) + " characters");
            label = truncateUtf8(std::move(label), DeviceLabels::kMaxLabel);
        }
        labels[i] = std::move(label);
    }
    const auto& sections = ini.sections();
    if (const auto it = sections.find(section); it != sections.end()) {
        for (const auto& [key, value] : it->second) {
            if (known.count(key) == 0) errors.push_back("[" + section + "] unknown key '" + key + "'");
        }
    }
}

template <std::size_t N>
void writeLabels(IniEditor& ini, const std::string& section, const std::string& prefix,
                 const std::array<std::string, N>& labels) {
    for (std::size_t i = 0; i < N; ++i) {
        const std::string key = prefix + std::to_string(i);
        if (labels[i].empty()) {
            ini.remove(section, key);
        } else {
            ini.set(section, key, labels[i]);
        }
    }
}

}  // namespace

const char* deviceKindName(const DeviceKind kind) {
    switch (kind) {
        case DeviceKind::Wheel: return "wheel";
        case DeviceKind::Pedals: return "pedals";
        case DeviceKind::Shifter: return "shifter";
        case DeviceKind::Handbrake: return "handbrake";
        case DeviceKind::ButtonBox: return "button box";
        case DeviceKind::Other: break;
    }
    return "other";
}

std::optional<DeviceKind> parseDeviceKind(const std::string_view text) {
    const std::string wanted = lower(text);
    for (const DeviceKind kind : kDeviceKinds) {
        if (wanted == deviceKindName(kind)) return kind;
    }
    return std::nullopt;
}

std::string deviceLabelsFileName(const std::uint16_t vendorId, const std::uint16_t productId) {
    char text[32];
    std::snprintf(text, sizeof(text), "%04x-%04x.ini", static_cast<unsigned>(vendorId), static_cast<unsigned>(productId));
    return text;
}

std::string deviceId(const std::uint16_t vendorId, const std::uint16_t productId) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04x:%04x", static_cast<unsigned>(vendorId), static_cast<unsigned>(productId));
    return text;
}

DeviceLabelsResult loadDeviceLabels(const IniDocument& ini) {
    DeviceLabelsResult result;
    DeviceLabels& labels = result.labels;
    labels.name = ini.get("device", "name").value_or("");
    if (const auto id = ini.get("device", "id")) {
        // The same strict reader as rigs and mappings (sscanf took "-1:0005" or "0x46:c262").
        if (const auto parsed = parseDeviceId(*id)) {
            labels.vendorId = parsed->vendorId;
            labels.productId = parsed->productId;
        } else {
            result.errors.push_back("[device] id = '" + *id + "': expected vvvv:pppp (hex)");
        }
    }
    if (const auto kind = ini.get("device", "kind")) {
        if (const auto parsed = parseDeviceKind(*kind)) {
            labels.kind = *parsed;
        } else {
            result.errors.push_back("[device] kind = '" + *kind +
                                    "': expected wheel, pedals, shifter, handbrake, button box or other");
        }
    }
    readLabels(ini, "axis_labels", "axis_", labels.axes, result.errors);
    readLabels(ini, "button_labels", "button_", labels.buttons, result.errors);
    return result;
}

std::string writeDeviceLabels(const std::string_view base, const DeviceLabels& labels) {
    IniEditor ini(base);
    ini.prepend("; WheelBri device: what it is and what its controls are called (the Rig tab writes this)");
    ini.set("device", "name", iniSafeValue(labels.name));   // from Windows or evdev: may hold ';' or '#'
    ini.set("device", "id", deviceId(labels.vendorId, labels.productId));
    ini.set("device", "kind", deviceKindName(labels.kind));
    writeLabels(ini, "axis_labels", "axis_", labels.axes);
    writeLabels(ini, "button_labels", "button_", labels.buttons);
    return ini.text();
}

std::vector<DeviceLabels> listDeviceLabels(const std::filesystem::path& folder) {
    std::vector<DeviceLabels> found;
    forEachEntry(folder, [&found](const std::filesystem::directory_entry& entry) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || entry.path().extension() != ".ini") return;
        const std::optional<std::string> text = readFileText(entry.path());
        if (!text) return;
        const auto parsed = IniDocument::parse(*text);
        const auto loaded = loadDeviceLabels(parsed.document);
        if (loaded.labels.vendorId == 0 && loaded.labels.productId == 0) return;
        found.push_back(loaded.labels);
    });
    std::sort(found.begin(), found.end(), [](const DeviceLabels& a, const DeviceLabels& b) {
        return lower(a.name) != lower(b.name) ? lower(a.name) < lower(b.name)
                                              : deviceId(a.vendorId, a.productId) < deviceId(b.vendorId, b.productId);
    });
    return found;
}

void applyDeviceLabels(const DeviceLabels& labels, MapperConfig& mapper) {
    mapper.sourceAxisLabels = labels.axes;
    mapper.sourceButtonLabels = labels.buttons;
}

}  // namespace wheelbri
