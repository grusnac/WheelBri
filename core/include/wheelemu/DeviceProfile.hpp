#pragma once

#include "wheelemu/WheelState.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wheelemu {

// Byte-for-byte layout of a Windows GUID, without depending on <windows.h>.
struct Guid {
    std::uint32_t data1 = 0;
    std::uint16_t data2 = 0;
    std::uint16_t data3 = 0;
    std::array<std::uint8_t, 8> data4{};

    friend bool operator==(const Guid&, const Guid&) = default;
};

[[nodiscard]] std::string toString(const Guid& guid);

struct DeviceIdentity {
    std::string shortName;              // for people: "G920" in logs and the --ui window
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string productName;            // what DirectInput reports as tszProductName
    float rotationDeg = 900.0F;

    // DirectInput's guidProduct for HID devices: {PIDVID-0000-0000-0000-504944564944} ("PIDVID").
    [[nodiscard]] Guid directInputProductGuid() const;
};

struct AxisRange {
    std::int32_t min = 0;
    std::int32_t max = 65535;
};

// WheelState encoded in the emulated device's own logical ranges.
struct TargetReport {
    std::int32_t steering = 0;
    std::int32_t throttle = 0;
    std::int32_t brake = 0;
    std::int32_t clutch = 0;
    std::uint32_t buttons = 0;
    std::int8_t hat = kHatCentered;

    friend bool operator==(const TargetReport&, const TargetReport&) = default;
};

// A wheel the emulator can impersonate. New targets (T300, G920, ...) are new implementations.
class IDeviceProfile {
public:
    virtual ~IDeviceProfile() = default;

    [[nodiscard]] virtual const DeviceIdentity& identity() const = 0;
    [[nodiscard]] virtual TargetReport encode(const WheelState& state) const = 0;

    // Label printed on the emulated wheel for a target button (0-based), or empty if it has none.
    // For display and logs only; nothing is decided by it.
    [[nodiscard]] virtual std::string_view buttonName(std::size_t /*index*/) const { return {}; }
};

struct DeviceProfileSpec {
    DeviceIdentity identity;
    AxisRange steeringRange;
    AxisRange pedalRange;
    bool pedalsReleasedAtMax = false;   // some wheels report a released pedal as the top of the range
    std::vector<std::string> buttonNames;  // index = target button, 0-based
};

// Data-driven profile: identity plus linear axis encodings.
class DeviceProfile final : public IDeviceProfile {
public:
    explicit DeviceProfile(DeviceProfileSpec spec);

    [[nodiscard]] const DeviceIdentity& identity() const override { return spec_.identity; }
    [[nodiscard]] TargetReport encode(const WheelState& state) const override;
    [[nodiscard]] std::string_view buttonName(std::size_t index) const override;

private:
    [[nodiscard]] std::int32_t encodePedal(float travel) const;

    DeviceProfileSpec spec_;
};

// Logitech G29 in PC mode. VID/PID are from GIMX core/haptic/common/ff_lg.h (0xc24f = G29 PC).
// The product name, axis encodings and button labels still need checking against a real G29 (plan: Phase 0).
[[nodiscard]] DeviceProfileSpec g29Spec();

// Logitech G920 Driving Force (Xbox One / PC) in PC mode. 0xc262 is the PID it enumerates with once G HUB
// or the kernel driver has switched it out of its Xbox-mode PID (0xc261). Product name, pedal encoding and
// button labels are unverified against a real G920. Its force feedback is HID++ rather than the classic
// Logitech protocol, which matters for Phase 3/4, not for DirectInput.
[[nodiscard]] DeviceProfileSpec g920Spec();

// Config names of the built-in profiles, for messages.
inline constexpr std::string_view kKnownProfiles = "g920, g29";

// Looks up a built-in profile by its config name ("g920", "g29"). Returns nullptr for unknown names.
[[nodiscard]] std::unique_ptr<IDeviceProfile> makeProfile(std::string_view name);

}  // namespace wheelemu
