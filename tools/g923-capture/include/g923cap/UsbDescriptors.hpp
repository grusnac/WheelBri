#pragma once

// USB and HID descriptors: enough to tell which interface and report carries what.

#include "g923cap/Pcap.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wheelbri::g923cap {

struct DeviceDescriptor {
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    std::uint16_t bcdDevice = 0;
    std::uint16_t bcdUsb = 0;
    Bytes raw;
};

struct Endpoint {
    std::uint8_t address = 0;
    std::uint8_t attributes = 0;
    std::uint16_t maxPacket = 0;
    std::uint8_t interval = 0;
};

struct Interface {
    std::uint8_t number = 0;
    std::uint8_t alternate = 0;
    std::uint8_t cls = 0;
    std::uint8_t subclass = 0;
    std::uint8_t protocol = 0;
    std::vector<Endpoint> endpoints;
    std::optional<std::uint16_t> reportDescriptorLength;
    Bytes hidRaw;
};

[[nodiscard]] std::optional<DeviceDescriptor> parseDeviceDescriptor(std::span<const std::uint8_t> raw);
[[nodiscard]] std::vector<Interface> parseConfiguration(std::span<const std::uint8_t> raw);

// A top-level collection: its usage, and per report ID the bits of input, output and feature (ID not counted).
struct Collection {
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
    std::map<std::uint8_t, std::array<std::uint32_t, 3>> reports;

    // Bytes on the wire, report ID included: which = 0 input, 1 output, 2 feature.
    [[nodiscard]] std::uint32_t reportBytes(std::uint8_t reportId, int which) const;
};

[[nodiscard]] std::vector<Collection> parseReportDescriptor(std::span<const std::uint8_t> raw);
[[nodiscard]] std::string pageName(std::uint16_t page);

}  // namespace wheelbri::g923cap
