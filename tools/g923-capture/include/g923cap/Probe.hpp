// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// S6: asks the real wheel itself, read-only. TrueForce (usage page 0xFFFD): 0x05 for parameters 0x00..0x3F. HID++
// (usage page 0xFF43): Root getProtocolVersion and getFeature, FeatureSet getCount/getFeatureId, DeviceFwVersion
// getEntityCount/getFwInfo, DeviceNameType getCount/getDeviceName/getDeviceType. Nothing else can be written:
// kSafeTrueForceTypes and safeHidpp() are checked before every write, and the tests pin both.
//
// The wheel answers HID++ requests sent in report 0x11 with report 0x12 (measured 2026-11-01), which Windows gives
// another handle: a HID++ transport reads both.

#include "g923cap/Pcap.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace wheelbri::g923cap {

inline constexpr std::array<std::uint8_t, 1> kSafeTrueForceTypes = {0x05};
// (feature id, function) pairs that only read.
[[nodiscard]] bool safeHidpp(std::uint16_t featureId, std::uint8_t function);
inline constexpr std::uint8_t kSoftwareId = 0x0B;   // ours, so the wheel's answers to G HUB are not taken for ours
inline constexpr std::uint8_t kDeviceIndex = 0xFF;  // a wheel on USB, not behind a receiver

struct Unsafe : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What the probe needs from a HID collection (WindowsHid, or a fake in the tests).
class Transport {
public:
    virtual ~Transport() = default;
    virtual void write(std::span<const std::uint8_t> report) = 0;
    // The next input report, or none within the time.
    virtual std::optional<Bytes> read(double timeoutS) = 0;
    [[nodiscard]] virtual std::size_t outputLength() const = 0;
};

// [01 00 00 00, type, seq, payload], `length` bytes; `seal`: a checksum after the payload.
[[nodiscard]] Bytes trueForceRequest(std::uint8_t type, std::uint8_t seq, std::span<const std::uint8_t> payload,
                                     std::size_t length, bool seal);
[[nodiscard]] Bytes hidppRequest(std::uint8_t featureIndex, std::uint16_t featureId, std::uint8_t function,
                                 std::span<const std::uint8_t> params, std::size_t length);

struct ProbeParameter {
    std::uint32_t bits = 0;
    bool wellFormed = false;
    std::string raw;
};
struct TrueForceProbe {
    std::map<int, ProbeParameter> parameters;
    std::vector<int> noAnswer;
    std::optional<bool> sealedRequests;   // whether answers came to requests with a checksum
    std::map<int, int> otherReports;      // report types seen while waiting
};
[[nodiscard]] TrueForceProbe probeTrueForce(Transport& t, double timeoutS = 0.3, int first = 0x00, int last = 0x3F);

struct HidppFeature {
    int index = 0;
    int type = 0;
    std::optional<int> version;
};
struct HidppProbe {
    std::optional<std::string> protocol;
    std::map<std::uint16_t, HidppFeature> features;
    std::vector<std::string> firmware;
    std::string name;
    std::optional<int> deviceType;
    std::vector<std::string> errors;
};
[[nodiscard]] HidppProbe probeHidpp(Transport& t, double timeoutS = 0.5);

struct ProbeRun {
    std::string when;
    std::string ghub;   // running / closed
    std::vector<std::string> collections;
    std::optional<TrueForceProbe> trueForce;
    std::optional<HidppProbe> hidpp;
    std::vector<std::string> problems;
};
[[nodiscard]] std::string probeMarkdown(const ProbeRun& run);

// Windows: finds the G923's collections, probes, writes probe-ghub-<ghub>.md in `out`. 0 done, 1 no wheel, 2 not
// Windows.
int runProbe(const std::string& out, const std::string& ghub);

}  // namespace wheelbri::g923cap
