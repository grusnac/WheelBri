#pragma once

#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

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

// A group of a target's buttons as the Mapping tab shows them ("FACE": A, B, X, Y), in a column of its own (Left,
// Right: groups stack down their column) or Below both columns (two buttons a row: the shifter's gears).
struct ButtonGroup {
    enum class Place { Left, Right, Below };
    std::string name;
    std::vector<std::size_t> buttons;   // target buttons (0-based), in the order shown
    Place place = Place::Left;

    friend bool operator==(const ButtonGroup&, const ButtonGroup&) = default;
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
    // How many buttons the emulated wheel reports (DirectInput's dwButtons, the HID report's button count), unnamed
    // gaps included: the G920's button 12 (1-based) exists but nothing presses it.
    [[nodiscard]] virtual std::size_t buttonCount() const { return 0; }
    // How the Mapping tab groups its named buttons (related ones together); empty: in number order.
    [[nodiscard]] virtual std::vector<ButtonGroup> buttonGroups() const { return {}; }
};

// Where each heading and named button goes on the Mapping tab: column 0 (left) or 1 (right), and its row from the top
// of the grid. A heading row is short (kButtonHeadingH), a button row kButtonRowH. Every named button appears once: one
// no group names goes Below, under "OTHER"; without groups, all are Below in number order (no heading).
inline constexpr float kButtonRowH = 20.0F;
inline constexpr float kButtonHeadingH = 14.0F;
struct ButtonCell {
    int column = 0;
    float y = 0.0F;                 // from the top of the grid
    bool heading = false;
    std::string text;               // a heading's name
    std::size_t button = 0;         // a button's index

    friend bool operator==(const ButtonCell&, const ButtonCell&) = default;
};
struct ButtonLayout {
    std::vector<ButtonCell> cells;
    float height = 0.0F;            // of the whole grid
};
[[nodiscard]] ButtonLayout layoutButtons(const IDeviceProfile& profile);

struct DeviceProfileSpec {
    DeviceIdentity identity;
    AxisRange steeringRange;
    AxisRange pedalRange;
    bool pedalsReleasedAtMax = false;   // some wheels report a released pedal as the top of the range
    std::vector<std::string> buttonNames;  // index = target button, 0-based; "" = a button nothing presses
    std::vector<ButtonGroup> buttonGroups; // the Mapping tab's groups (IDeviceProfile::buttonGroups)
};

// Data-driven profile: identity plus linear axis encodings.
class DeviceProfile final : public IDeviceProfile {
public:
    explicit DeviceProfile(DeviceProfileSpec spec);

    [[nodiscard]] const DeviceIdentity& identity() const override { return spec_.identity; }
    [[nodiscard]] TargetReport encode(const WheelState& state) const override;
    [[nodiscard]] std::string_view buttonName(std::size_t index) const override;
    [[nodiscard]] std::size_t buttonCount() const override { return spec_.buttonNames.size(); }
    [[nodiscard]] std::vector<ButtonGroup> buttonGroups() const override { return spec_.buttonGroups; }

private:
    [[nodiscard]] std::int32_t encodePedal(float travel) const;

    DeviceProfileSpec spec_;
    std::uint32_t buttonMask_ = 0;   // the named buttons: only they reach the game
};

// The G920's buttons with the Driving Force Shifter: 11 on the wheel, an unused 12th, then the shifter's gears 1-6 and
// reverse (Logitech's shifter FAQ: G920 buttons 13-18 and 19, 1-based), so 19 in all.
inline constexpr std::size_t kG920Buttons = 19;
inline constexpr std::size_t kG920FirstGear = 12;   // 0-based: Gear 1 .. Gear 6 are 12..17, Gear R is 18

// Logitech G920 Driving Force (Xbox One / PC) in PC mode. 0xc262 is the PID it enumerates with once G HUB
// or the kernel driver has switched it out of its Xbox-mode PID (0xc261). Product name, pedal encoding and
// button labels are unverified against a real G920. Its force feedback is HID++ rather than the classic
// Logitech protocol, which matters for Phase 3/4, not for DirectInput.
[[nodiscard]] DeviceProfileSpec g920Spec();

// The G25's buttons in native mode (046D:C299): 19, the shifter's included (docs/g25-target-plan.md). Logitech's own
// numbering puts the gears at 8..14 (0-based: Gear 1 .. Gear 6, then Gear R); the order of the other 12 (4 black and
// 4 red shifter buttons, 2 paddles, 2 wheel buttons) is not documented and is a guess until a real G25 is captured.
inline constexpr std::size_t kG25Buttons = 19;
inline constexpr std::size_t kG25FirstGear = 8;

// Logitech G25 Racing Wheel in native mode (046D:C299, after the mode switch a real one gets from Logitech's driver).
// The axes are presented like the G920's (wheel X, throttle Y, brake Rz, clutch Slider, released pedals at the top of
// the range), which is also what a G25 with separate pedals is reported to show in DirectInput; unverified against a
// real one, like the button order above. The 900 degrees are its full lock.
[[nodiscard]] DeviceProfileSpec g25Spec();

// Config names of the built-in profiles, for messages: the wheels WheelBri emulates (a virtual device and the shim both
// present them). A new target is a new profile here plus its support (driver, shim) first.
inline constexpr std::string_view kKnownProfiles = "g920, g25";

// Looks up a built-in profile by its config name ("g920", "g25"). Returns nullptr for unknown names.
[[nodiscard]] std::unique_ptr<IDeviceProfile> makeProfile(std::string_view name);

}  // namespace wheelbri
