#include "wheelbri/HideIds.hpp"

#include "wheelbri/HidWheelDevice.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace wheelbri::hideids {

namespace {

std::optional<std::uint16_t> hex4(const std::string_view text) {
    if (text.size() < 4) return std::nullopt;
    unsigned value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return std::nullopt;
        value = value * 16 + digit;
    }
    return static_cast<std::uint16_t>(value);
}

}  // namespace

std::optional<DeviceId> hidDeviceIdOf(const std::string_view instanceId) {
    constexpr std::string_view kVid = "HID\\VID_";
    constexpr std::string_view kPid = "&PID_";
    if (instanceId.size() < kVid.size() + 4 + kPid.size() + 4) return std::nullopt;
    if (!equalsIgnoreCase(instanceId.substr(0, kVid.size()), kVid)) return std::nullopt;
    const auto vendor = hex4(instanceId.substr(kVid.size()));
    const std::size_t pidAt = kVid.size() + 4;
    if (!equalsIgnoreCase(instanceId.substr(pidAt, kPid.size()), kPid)) return std::nullopt;
    const auto product = hex4(instanceId.substr(pidAt + kPid.size()));
    if (!vendor || !product) return std::nullopt;
    return DeviceId{*vendor, *product};
}

bool isGameController(const std::vector<std::string>& compatibleIds) {
    for (const std::string& id : compatibleIds) {
        if (equalsIgnoreCase(id, "HID_DEVICE_SYSTEM_GAME") || equalsIgnoreCase(id, "HID_DEVICE_UP:0001_U:0004") ||
            equalsIgnoreCase(id, "HID_DEVICE_UP:0001_U:0005") || equalsIgnoreCase(id, "HID_DEVICE_UP:0001_U:0008")) {
            return true;
        }
        constexpr std::string_view kSimulation = "HID_DEVICE_UP:0002_";
        if (id.size() >= kSimulation.size() && equalsIgnoreCase(std::string_view(id).substr(0, kSimulation.size()), kSimulation)) {
            return true;
        }
    }
    return false;
}

std::string neverHiddenUsage(const std::vector<std::string>& compatibleIds) {
    struct Rule {
        std::string_view id;
        bool prefix;
        std::string_view what;
    };
    static constexpr Rule kRules[] = {
        {"HID_DEVICE_SYSTEM_KEYBOARD", false, "a keyboard"},
        {"HID_DEVICE_SYSTEM_MOUSE", false, "a mouse"},
        {"HID_DEVICE_SYSTEM_CONTROL", false, "system controls"},
        {"HID_DEVICE_SYSTEM_CONSUMER", false, "consumer controls"},
        {"HID_DEVICE_UP:0001_U:0001", false, "a pointer"},
        {"HID_DEVICE_UP:0001_U:0002", false, "a mouse"},
        {"HID_DEVICE_UP:0001_U:0006", false, "a keyboard"},
        {"HID_DEVICE_UP:0001_U:0007", false, "a keypad"},
        {"HID_DEVICE_UP:0001_U:0080", false, "system controls"},
        {"HID_DEVICE_UP:000C_", true, "consumer controls"},
        {"HID_DEVICE_UP:000D_", true, "a digitizer"},
    };
    for (const std::string& id : compatibleIds) {
        for (const Rule& rule : kRules) {
            const bool match = rule.prefix ? id.size() >= rule.id.size() &&
                                                 equalsIgnoreCase(std::string_view(id).substr(0, rule.id.size()), rule.id)
                                           : equalsIgnoreCase(id, rule.id);
            if (match) return std::string(rule.what) + " (" + id + ")";
        }
    }
    return {};
}

std::string instanceIdOfHidPath(std::string_view path) {
    constexpr std::string_view kPrefix = "\\\\?\\";
    if (path.size() >= kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) path.remove_prefix(kPrefix.size());
    if (path.size() < 4 || !equalsIgnoreCase(path.substr(0, 4), "hid#")) return {};
    const std::size_t guid = path.rfind("#{");
    if (guid != std::string_view::npos) path = path.substr(0, guid);
    std::string id;
    id.reserve(path.size());
    for (const char c : path) {
        id += c == '#' ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return id;
}

bool mayHide(const DeviceId& id) {
    return !id.empty() && !isVirtualWheelId(id.vendorId, id.productId);
}

std::string deviceIdText(const DeviceId& id) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04x:%04x", id.vendorId, id.productId);
    return text;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool containsIgnoreCase(std::string_view text, std::string_view part) {
    if (part.size() > text.size()) return false;
    for (std::size_t i = 0; i + part.size() <= text.size(); ++i) {
        if (equalsIgnoreCase(text.substr(i, part.size()), part)) return true;
    }
    return false;
}

}  // namespace wheelbri::hideids
