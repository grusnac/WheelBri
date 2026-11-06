// SPDX-License-Identifier: GPL-3.0-or-later
#include "g923cap/UsbDescriptors.hpp"

#include <cstdio>

namespace wheelbri::g923cap {

std::optional<DeviceDescriptor> parseDeviceDescriptor(const std::span<const std::uint8_t> raw) {
    if (raw.size() < 18 || raw[1] != 0x01) return std::nullopt;
    DeviceDescriptor d;
    d.bcdUsb = static_cast<std::uint16_t>(raw[2] | raw[3] << 8);
    d.vendor = static_cast<std::uint16_t>(raw[8] | raw[9] << 8);
    d.product = static_cast<std::uint16_t>(raw[10] | raw[11] << 8);
    d.bcdDevice = static_cast<std::uint16_t>(raw[12] | raw[13] << 8);
    d.raw.assign(raw.begin(), raw.begin() + 18);
    return d;
}

std::vector<Interface> parseConfiguration(const std::span<const std::uint8_t> raw) {
    std::vector<Interface> interfaces;
    std::size_t pos = 0;
    Interface* current = nullptr;
    while (pos + 2 <= raw.size()) {
        const std::size_t length = raw[pos];
        const std::uint8_t kind = raw[pos + 1];
        if (length < 2) break;
        const std::span<const std::uint8_t> d = raw.subspan(pos, std::min(length, raw.size() - pos));
        if (kind == 0x04 && d.size() >= 9) {
            interfaces.push_back({d[2], d[3], d[5], d[6], d[7], {}, std::nullopt, {}});
            current = &interfaces.back();
        } else if (kind == 0x05 && d.size() >= 7 && current != nullptr) {
            current->endpoints.push_back({d[2], d[3], static_cast<std::uint16_t>(d[4] | d[5] << 8), d[6]});
        } else if (kind == 0x21 && d.size() >= 9 && current != nullptr) {
            current->hidRaw.assign(d.begin(), d.end());
            const std::size_t n = d[5];
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t at = 6 + k * 3;
                if (at + 3 <= d.size() && d[at] == 0x22) {
                    current->reportDescriptorLength = static_cast<std::uint16_t>(d[at + 1] | d[at + 2] << 8);
                }
            }
        }
        pos += length;
    }
    return interfaces;
}

std::uint32_t Collection::reportBytes(const std::uint8_t reportId, const int which) const {
    const auto it = reports.find(reportId);
    const std::uint32_t bits = it == reports.end() ? 0 : it->second[static_cast<std::size_t>(which)];
    return (bits + 7) / 8 + (reportId != 0 ? 1 : 0);
}

std::vector<Collection> parseReportDescriptor(const std::span<const std::uint8_t> raw) {
    std::vector<Collection> collections;
    std::uint32_t usagePage = 0;
    std::uint32_t reportId = 0;
    std::uint32_t reportSize = 0;
    std::uint32_t reportCount = 0;
    std::vector<std::uint32_t> usages;
    std::vector<std::array<std::uint32_t, 4>> stack;
    int depth = 0;
    Collection* top = nullptr;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::uint8_t prefix = raw[pos];
        if (prefix == 0xFE) {   // long item
            if (pos + 1 >= raw.size()) break;
            pos += 3 + static_cast<std::size_t>(raw[pos + 1]);
            continue;
        }
        static constexpr std::size_t kSizes[] = {0, 1, 2, 4};
        const std::size_t size = kSizes[prefix & 3];
        const int kind = (prefix >> 2) & 3;
        const int tag = prefix >> 4;
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < size && pos + 1 + i < raw.size(); ++i) {
            value |= static_cast<std::uint32_t>(raw[pos + 1 + i]) << (8 * i);
        }
        pos += 1 + size;
        if (kind == 1) {   // global
            if (tag == 0x0) usagePage = value;
            else if (tag == 0x7) reportSize = value;
            else if (tag == 0x8) reportId = value;
            else if (tag == 0x9) reportCount = value;
            else if (tag == 0xA) stack.push_back({usagePage, reportId, reportSize, reportCount});
            else if (tag == 0xB && !stack.empty()) {
                usagePage = stack.back()[0];
                reportId = stack.back()[1];
                reportSize = stack.back()[2];
                reportCount = stack.back()[3];
                stack.pop_back();
            }
        } else if (kind == 2) {   // local
            if (tag == 0x0) usages.push_back(size == 4 ? value : (usagePage << 16) | value);
        } else if (kind == 0) {   // main
            if (tag == 0xA) {   // collection
                if (depth == 0) {
                    const std::uint32_t first = usages.empty() ? (usagePage << 16) : usages.front();
                    collections.push_back({static_cast<std::uint16_t>(first >> 16),
                                           static_cast<std::uint16_t>(first & 0xFFFF), {}});
                    top = &collections.back();
                }
                ++depth;
            } else if (tag == 0xC) {
                depth = depth > 0 ? depth - 1 : 0;
            } else if ((tag == 0x8 || tag == 0x9 || tag == 0xB) && top != nullptr) {
                const int which = tag == 0x8 ? 0 : tag == 0x9 ? 1 : 2;
                auto& bits = top->reports[static_cast<std::uint8_t>(reportId)];
                bits[static_cast<std::size_t>(which)] += reportSize * reportCount;
            }
            usages.clear();
        }
    }
    return collections;
}

std::string pageName(const std::uint16_t page) {
    switch (page) {
        case 0x01: return "Generic Desktop";
        case 0x0F: return "Physical Interface (PID)";
        case 0xFF00: return "vendor FF00";
        case 0xFF43: return "Logitech HID++ (FF43)";
        case 0xFFFD: return "TrueForce (FFFD)";
        default: {
            char b[16];
            std::snprintf(b, sizeof(b), "0x%04X", page);
            return b;
        }
    }
}

}  // namespace wheelbri::g923cap
