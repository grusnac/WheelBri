#pragma once

#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <functional>
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

// The combined pedals axis of a target whose pedals rest at the top of the range (a G25's, Logitech's "Combined
// pedals"): the middle at rest, the throttle pulls it down and the brake pushes it up (both pressed: they cancel), as
// Race Driver: GRID's G25 preset reads Slider 0 (its lower half throttle, its upper half brake). 0..65535.
[[nodiscard]] constexpr std::int32_t combinedPedals(const TargetReport& report) {
    const std::int64_t value = (std::int64_t{65535} + report.throttle - report.brake) / 2;
    return static_cast<std::int32_t>(value < 0 ? 0 : value > 65535 ? 65535 : value);
}

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
    bool folded = false;            // a heading standing for its whole group (folded: one kButtonRowH row)

    friend bool operator==(const ButtonCell&, const ButtonCell&) = default;
};
struct ButtonLayout {
    std::vector<ButtonCell> cells;
    float height = 0.0F;            // of the whole grid
};
// `fold` says which named groups show as their heading alone (one kButtonRowH row, `folded`, in its column; a Below
// group's in column 0): the Mapping tab folds a group with nothing mapped. Null: none.
[[nodiscard]] ButtonLayout layoutButtons(const IDeviceProfile& profile,
                                         const std::function<bool(const ButtonGroup&)>& fold = {});

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

// The G25's buttons in native mode (046D:C299): 19, the shifter's included (README, "Targets"). Logitech's own
// numbering puts the gears at 8..14 (0-based: Gear 1 .. Gear 6, then Gear R); the order of the other 12 (4 black and
// 4 red shifter buttons, 2 paddles, 2 wheel buttons) is not documented and is a guess until a real G25 is captured.
inline constexpr std::size_t kG25Buttons = 19;
inline constexpr std::size_t kG25FirstGear = 8;

// Logitech G25 Racing Wheel in native mode (046D:C299, after the mode switch a real one gets from Logitech's driver).
// The axes are presented like the G920's (wheel X, throttle Y, brake Rz, clutch Slider, released pedals at the top of
// the range), which is also what a G25 with separate pedals is reported to show in DirectInput; unverified against a
// real one, like the button order above. The 900 degrees are its full lock.
[[nodiscard]] DeviceProfileSpec g25Spec();

// The G27's buttons in native mode (046D:C29B): 23 (README, "Targets"), the G25's 19 and 4 more on the wheel.
// The gears are at 8..14 as on the G25 (Logitech's numbering); the rest is from users' reports of what games and
// Logitech's Profiler show, not from Logitech, and unverified until a real G27 is captured: the shifter's 4 red buttons
// 0..3, paddles R 4 and L 5, the wheel's top buttons R 6 and L 7, the shifter's 4 black buttons 15..18, then the
// wheel's lower buttons R 19, L 20, R 21, L 22 (top to bottom).
inline constexpr std::size_t kG27Buttons = 23;
inline constexpr std::size_t kG27FirstGear = 8;

// Logitech G27 Racing Wheel in native mode (046D:C29B): presented like the G25 (the same axes, pedals released at the
// top, 900 degrees), with its 23 buttons. Its rev lights are not emulated (games drive them through Logitech's SDK).
[[nodiscard]] DeviceProfileSpec g27Spec();

// The G923 for Xbox and PC in PC mode (046D:C26E, README, "Targets"): 24 buttons, the G920's 19 (the same
// HID++ wheel family: Linux's hid-logitech-hidpp drives both with the G920's quirks) and the 5 the G920 lacks, after
// them: Plus 19, Minus 20, Dial Right 21, Dial Left 22, Dial Enter 23. Logitech does not document the order: the
// G920's part follows the G920, the last 5 the G29's order (+, -, dial right, dial left, enter); unverified until a
// real G923 is captured.
inline constexpr std::size_t kG923Buttons = 24;
inline constexpr std::size_t kG923FirstGear = kG920FirstGear;

// Logitech G923 Racing Wheel for Xbox One and PC in PC mode (046D:C26E; Xbox mode is C26D, which Logitech's driver or
// Linux's xpad switches out of): presented like the G920 (the same axes, pedals released at the top, 900 degrees),
// with its 24 buttons. Its rev lights and TrueForce are not emulated.
[[nodiscard]] DeviceProfileSpec g923Spec();

// Config names of the built-in profiles, for messages: the wheels WheelBri emulates (a virtual device and the shim both
// present them). A new target is a new profile here plus its support (driver, shim) first.
inline constexpr std::string_view kKnownProfiles = "g25, g27, g920, g923";

// Looks up a built-in profile by its config name ("g920", "g25", "g27", "g923"). Returns nullptr for unknown names.
[[nodiscard]] std::unique_ptr<IDeviceProfile> makeProfile(std::string_view name);

}  // namespace wheelbri
