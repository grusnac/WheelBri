// SPDX-License-Identifier: GPL-3.0-or-later
#include "g923cap/Protocol.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace wheelbri::g923cap {

const char* otherG923Name(const std::uint16_t product) {
    switch (product) {
        case 0xC26D: return "G923 for Xbox One and PC, Xbox mode (as recorded by others)";
        case 0xC266: return "G923 for PlayStation and PC (PS mode)";
        case 0xC267: return "G923 for PlayStation and PC (PC mode)";
        default: return nullptr;
    }
}

std::optional<Target> rehearsalTarget() {
    const char* text = std::getenv("G923_TARGET");
    if (text == nullptr || *text == '\0') return std::nullopt;
    const std::string s(text);
    const std::size_t colon = s.find(':');
    Target t;
    t.vendor = static_cast<std::uint16_t>(std::strtoul(s.substr(0, colon).c_str(), nullptr, 16));
    if (colon != std::string::npos) {
        const std::string pid = s.substr(colon + 1);
        if (!pid.empty() && pid != "any") t.product = static_cast<std::uint16_t>(std::strtoul(pid.c_str(), nullptr, 16));
    }
    return t;
}

bool isTarget(const std::uint16_t vendor, const std::uint16_t product) {
    if (const auto r = rehearsalTarget()) return vendor == r->vendor && (!r->product || product == *r->product);
    return vendor == kLogitech && (product == kG923 || otherG923Name(product) != nullptr);
}

bool isTrueForceReport(const std::span<const std::uint8_t> d) {
    return d.size() >= 6 && d[0] == 0x01 && d[1] == 0 && d[2] == 0 && d[3] == 0;
}

bool sealedAt(const std::span<const std::uint8_t> d, const std::size_t at) {
    if (at >= d.size() || at < 4) return false;
    unsigned sum = 0;
    for (std::size_t i = 4; i <= at; ++i) sum += d[i];
    return (sum & 0xFFU) == 0;
}

std::optional<std::size_t> sealOf(const std::uint8_t type) {
    switch (type) {
        case 0x05: return 13;
        case 0x07: return 25;
        case 0x02:
        case 0x10:
        case 0x11:
        case 0x14: return 33;
        default: return std::nullopt;
    }
}

std::optional<std::size_t> findSeal(const std::span<const std::uint8_t> d) {
    if (d.size() < 7) return std::nullopt;
    std::size_t last = d.size() - 1;
    while (last > 5 && d[last] == 0) --last;
    if (last > 5 && sealedAt(d, last)) return last;
    return std::nullopt;
}

bool isStatusType(const std::uint8_t type) { return type == 0x02 || type == 0x10 || type == 0x11 || type == 0x14; }

std::span<const std::uint8_t> expectedAnswers(const std::uint8_t requestType) {
    static constexpr std::array<std::uint8_t, 1> k05 = {0x05};
    static constexpr std::array<std::uint8_t, 1> k07 = {0x07};
    static constexpr std::array<std::uint8_t, 1> k10 = {0x10};
    // The operating range: a real G923 answers with a plain status of its sequence (2026-11-01, S2 frames 251 ->
    // 261, 489 -> 493, 499 -> 507); 0x11 and 0x14 are what Logitech's DLL also takes.
    static constexpr std::array<std::uint8_t, 3> kRange = {0x02, 0x11, 0x14};
    static constexpr std::array<std::uint8_t, 1> k02 = {0x02};
    switch (requestType) {
        case 0x05: return k05;
        case 0x07: return k07;
        case 0x06: return k10;
        case 0x0E: return kRange;
        case 0x03:
        case 0x04: return k02;
        default: return {};
    }
}

std::string typeName(const std::uint8_t type) {
    switch (type) {
        case 0x01: return "stream (torque + texture)";
        case 0x03: return "start session";
        case 0x04: return "stop session";
        case 0x05: return "parameter read";
        case 0x06: return "0x06 (status 10 answer)";
        case 0x07: return "slots";
        case 0x0C: return "damping";
        case 0x0E: return "operating range";
        default: return "";
    }
}

std::uint32_t u24At(const std::span<const std::uint8_t> d, const std::size_t at) {
    return static_cast<std::uint32_t>(d[at] | d[at + 1] << 8 | d[at + 2] << 16);
}

std::uint32_t u32At(const std::span<const std::uint8_t> d, const std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
           static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}

bool isHidpp(const std::span<const std::uint8_t> d) {
    return d.size() >= 4 && (d[0] == 0x10 || d[0] == 0x11 || d[0] == 0x12) &&
           (d.size() == 7 || d.size() == 20 || d.size() == 64);
}

bool isHidppError(const std::span<const std::uint8_t> a) { return a.size() >= 6 && (a[2] == 0xFF || a[2] == 0x8F); }

bool hidppAnswers(const std::span<const std::uint8_t> q, const std::span<const std::uint8_t> a) {
    if (a.size() < 5 || q.size() < 4 || a[1] != q[1]) return false;
    if (isHidppError(a)) return a[3] == q[2] && a[4] == q[3];
    return a[2] == q[2] && a[3] == q[3];
}

std::string featureName(const std::optional<std::uint16_t> id) {
    if (!id) return "?";
    static const std::map<std::uint16_t, const char*> kNames = {
        {0x0000, "Root"}, {0x0001, "FeatureSet"}, {0x0002, "FeatureInfo"}, {0x0003, "DeviceFwVersion"},
        {0x0005, "DeviceNameType"}, {0x0007, "DeviceFriendlyName"}, {0x0020, "ConfigChange"},
        {0x1E00, "EnableHiddenFeatures"}, {0x1802, "Device reset"}, {0x18A1, "LED test"},
        {0x1F1F, "Firmware properties"}, {0x8123, "ForceFeedback"}, {0x8127, "Peripheral tuning (?)"}};
    char b[16];
    std::snprintf(b, sizeof(b), "0x%04X", *id);
    const auto it = kNames.find(*id);
    return it == kNames.end() ? std::string(b) : std::string(b) + " " + it->second;
}

std::string hidppErrorName(const std::uint8_t code) {
    static constexpr const char* kNames[] = {"no error", "unknown", "invalid argument", "out of range",
                                             "hardware error", "Logitech internal", "invalid feature index",
                                             "invalid function", "busy", "unsupported"};
    if (code < std::size(kNames)) return kNames[code];
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02X", code);
    return b;
}

std::string hexOf(const std::span<const std::uint8_t> d, const std::size_t n) {
    std::string s;
    const std::size_t shown = std::min(n, d.size());
    char b[4];
    for (std::size_t i = 0; i < shown; ++i) {
        std::snprintf(b, sizeof(b), "%02x", d[i]);
        if (i) s += ' ';
        s += b;
    }
    if (d.size() > shown) {
        bool zero = true;
        for (std::size_t i = shown; i < d.size(); ++i) zero = zero && d[i] == 0;
        s += " \xE2\x80\xA6 (+" + std::to_string(d.size() - shown) + " bytes" + (zero ? ", all zero" : "") + ")";
    }
    return s;
}

std::string hexPlain(const std::span<const std::uint8_t> d) {
    std::string s;
    char b[4];
    for (const std::uint8_t c : d) {
        std::snprintf(b, sizeof(b), "%02x", c);
        s += b;
    }
    return s;
}

std::string asciiOf(const std::span<const std::uint8_t> d) {
    std::string s;
    for (const std::uint8_t c : d) s += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    return s;
}

std::string numberText(const double v) {
    if (std::isnan(v)) return "nan";
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

}  // namespace wheelbri::g923cap
