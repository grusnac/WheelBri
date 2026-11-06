// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/AxisAudit.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wheelbri {

namespace {

std::uint32_t readU32(const std::span<const std::uint8_t> bytes, const std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24U);
}

float readFloat(const std::span<const std::uint8_t> bytes, const std::size_t at) {
    const std::uint32_t bits = readU32(bytes, at);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string lower(const std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::optional<RevLights> decodeRevLights(const std::uint32_t command, const std::span<const std::uint8_t> in) {
    constexpr std::size_t kSize = 20;
    if (command != 0 || in.size() != kSize || readU32(in, 0) != kSize || readU32(in, 4) != 1) {
        return std::nullopt;
    }
    const RevLights lights{readFloat(in, 8), readFloat(in, 12), readFloat(in, 16)};
    if (!std::isfinite(lights.rpm) || !std::isfinite(lights.firstLight) || !std::isfinite(lights.redline)) {
        return std::nullopt;
    }
    return lights;
}

std::string hexBytes(const std::span<const std::uint8_t> bytes, const std::size_t max) {
    std::string out;
    const std::size_t shown = bytes.size() < max ? bytes.size() : max;
    for (std::size_t i = 0; i < shown; ++i) {
        std::array<char, 4> hex{};
        std::snprintf(hex.data(), hex.size(), "%02x", bytes[i]);
        if (i != 0) out += ' ';
        out += hex.data();
    }
    if (shown < bytes.size()) {
        out += " ... (" + std::to_string(bytes.size()) + " bytes)";
    }
    return out;
}

bool setPropertyCouldRemap(const std::uint32_t propertyId) {
    switch (propertyId) {
        case 1:   // DIPROP_BUFFERSIZE
        case 2:   // DIPROP_AXISMODE (absolute or relative)
        case 3:   // DIPROP_GRANULARITY
        case 4:   // DIPROP_RANGE
        case 5:   // DIPROP_DEADZONE
        case 6:   // DIPROP_SATURATION
        case 7:   // DIPROP_FFGAIN
        case 9:   // DIPROP_AUTOCENTER
            return false;
        default:
            return true;
    }
}

bool isInputModule(const std::string_view fileName) {
    const std::string name = lower(fileName);
    const auto startsWith = [&](const std::string_view prefix) { return name.rfind(prefix, 0) == 0; };
    const auto contains = [&](const std::string_view part) { return name.find(part) != std::string::npos; };
    return startsWith("logi") || startsWith("lgs") || startsWith("lg_") || contains("ghub") ||
           contains("logitech") || contains("steeringwheel") || startsWith("xinput") || startsWith("sdl") ||
           name == "hid.dll" || name == "winusb.dll" || name == "windows.gaming.input.dll" || name == "dinput.dll";
}

std::uint64_t AxisAudit::record(const std::string_view kind, const Risk risk) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++total_;
    for (Entry& entry : entries_) {
        if (entry.kind == kind) {
            return ++entry.count;
        }
    }
    entries_.push_back(Entry{std::string(kind), 1, risk});
    return 1;
}

std::string AxisAudit::summary() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string kinds;
    std::string risky;
    for (const Entry& entry : entries_) {
        const std::string item = entry.kind + " x" + std::to_string(entry.count);
        kinds += (kinds.empty() ? "" : ", ") + item;
        if (entry.risk == Risk::CouldRemap) {
            risky += (risky.empty() ? "" : ", ") + item;
        }
    }
    std::string out = std::to_string(total_) + " calls of " + std::to_string(entries_.size()) + " kinds; ";
    out += risky.empty() ? "none could remap axes" : "COULD REMAP AXES: " + risky;
    if (!kinds.empty()) {
        out += ". Kinds: " + kinds;
    }
    return out;
}

std::uint64_t AxisAudit::couldRemapCalls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t calls = 0;
    for (const Entry& entry : entries_) {
        if (entry.risk == Risk::CouldRemap) calls += entry.count;
    }
    return calls;
}

std::uint64_t AxisAudit::totalCalls() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return total_;
}

}  // namespace wheelbri
