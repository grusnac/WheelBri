#pragma once

#include "wheelbri/LogLevel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri {

class HidPpFfb;

// HID++ 2.0, device side: what Logitech's software (G HUB) asks a Logitech wheel over its vendor reports, answered for
// a virtual wheel that has them (VirtualWheel::hidpp, docs/targets.md). Written from the
// protocol's public description (Logitech's HID++ 2.0 documents, MIT-licensed tools); nothing is copied from GPL code.
//
// A request and its answer are one report each, the same size: long (report ID 0x11, 20 bytes) or very long (0x12, 64
// bytes): [report ID, device index, feature index, function << 4 | software id, parameters...]. The answer repeats the
// first four bytes; an error answer is [report ID, device index, 0xFF, feature index, function | sw id, error code].
// A HID++ 1.0 request (feature index 0x80..0x83: register access) gets 1.0's error [.., 0x8F, sub id, address, 0x01].
//
// Features (index: id):
//   0 0x0000 Root            getFeature(id) -> index, type, version (index 0 = not there); getProtocolVersion -> 4.2
//   1 0x0001 FeatureSet      getCount, getFeatureId(index)
//   2 0x0003 DeviceFwVersion getEntityCount (unit id, USB, model id), getFwInfo
//   3 0x0005 DeviceNameType  getCount, getDeviceName(char index), getDeviceType
//   4 0x8123 ForceFeedback   info, aperture (rotation) and global gains kept and reported to `settings`; effects go
//                            to the HidPpFfb set with setForceFeedback (played), else they are only acknowledged
// Anything else: an error, and a line in the log saying what was asked (that is how the next features are found).
class HidPp {
public:
    using Logger = std::function<void(LogLevel, const std::string&)>;
    // What Logitech's software set: the rotation in degrees and the global gain (0..1). Not applied by WheelBri yet.
    using Settings = std::function<void(int rotationDeg, float gain)>;

    struct Identity {
        std::string name;          // "G923 Racing Wheel for Xbox One and PC"
        std::uint16_t productId = 0;
        std::uint32_t unitId = 0;  // 0x0003's unit id (a serial number of sorts)
        std::uint8_t deviceType = 0;
    };

    HidPp(Identity identity, Logger log, Settings settings = {});

    // Where 0x8123's effects go (it must outlive this): Logitech's force feedback driver sends a TrueForce game's force
    // this way. Without one they are acknowledged and logged only.
    void setForceFeedback(HidPpFfb* ffb);

    static constexpr std::uint8_t kLongId = 0x11;
    static constexpr std::size_t kLongSize = 20;
    static constexpr std::uint8_t kVeryLongId = 0x12;
    static constexpr std::size_t kVeryLongSize = 64;
    [[nodiscard]] static bool isHidPpReport(const std::uint8_t reportId) {
        return reportId == kLongId || reportId == kVeryLongId;
    }

    // A request from the host (`report` with its report ID): the answer to send back as an input report, or nothing
    // when it is not a HID++ request at all.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> handle(std::span<const std::uint8_t> report,
                                                                  std::int64_t nowMs);

    // HID++ 2.0 error codes.
    static constexpr std::uint8_t kErrInvalidArgument = 0x02, kErrOutOfRange = 0x03, kErrInvalidFeatureIndex = 0x06,
                                  kErrInvalidFunction = 0x07;
    // Feature ids.
    static constexpr std::uint16_t kRoot = 0x0000, kFeatureSet = 0x0001, kFwVersion = 0x0003, kNameType = 0x0005,
                                   kForceFeedback = 0x8123;
    // 0x8123: a guess from Linux's log of a real G923 ("num_effects=63" after one reserved slot).
    static constexpr std::uint8_t kEffectSlots = 64;

    // The rig's own rotation (target_rotation_deg, from the service's state; 40..2520, anything else ignored): what
    // "get aperture" reports from then on, as a real wheel reports the rotation it turns. ACC reads it and divides its
    // steering by it, and sets it back (2026-10-28).
    void setWheelRotation(int degrees);
    // The rotation software last set ("set aperture"; WheelBri does not apply it).
    [[nodiscard]] int rotationDeg() const;
    [[nodiscard]] float gain() const;
    [[nodiscard]] std::uint64_t requests() const;

private:
    struct Feature {
        std::uint16_t id;
        std::uint8_t version;
    };
    using Params = std::span<const std::uint8_t>;
    using Out = std::span<std::uint8_t>;   // the answer's parameters, zeroed

    // The answer's parameters, or an error code.
    std::optional<std::uint8_t> answer(std::uint8_t featureIndex, std::uint8_t function, Params in, Out out);
    std::optional<std::uint8_t> root(std::uint8_t function, Params in, Out out);
    std::optional<std::uint8_t> featureSet(std::uint8_t function, Params in, Out out) const;
    std::optional<std::uint8_t> fwVersion(std::uint8_t function, Params in, Out out) const;
    std::optional<std::uint8_t> nameType(std::uint8_t function, Params in, Out out) const;
    std::optional<std::uint8_t> forceFeedback(std::uint8_t function, Params in, Out out);
    // Once per (feature, function): what Logitech's software asked, at info; every request at debug.
    void note(std::uint8_t featureIndex, std::uint8_t function, const std::string& what);
    // A 0x8123 call with its parameters: the first kShownCalls of each function at info, the rest at debug.
    void showCall(std::uint8_t function, const std::string& what, Params in);
    [[nodiscard]] static std::string hex(std::span<const std::uint8_t> bytes);

    Identity identity_;
    Logger log_;
    Settings settings_;
    std::vector<Feature> features_;
    mutable std::mutex mutex_;
    std::set<std::pair<std::uint8_t, std::uint8_t>> seen_;
    std::set<std::uint16_t> askedFor_;   // feature ids asked for that are not there (logged once each)
    int rotationDeg_ = 900;
    int wheelRotationDeg_ = 0;   // 0: not known yet
    std::uint16_t gain_ = 0xFFFF;
    std::uint16_t boost_ = 0;
    std::uint8_t nextSlot_ = 1;
    HidPpFfb* ffb_ = nullptr;
    std::int64_t nowMs_ = 0;   // the request's time, for the effects
    std::array<int, 16> shown_{};   // 0x8123 calls logged in full, per function (kShownCalls each)
    static constexpr int kShownCalls = 20;
    std::uint64_t requests_ = 0;
};

}  // namespace wheelbri
