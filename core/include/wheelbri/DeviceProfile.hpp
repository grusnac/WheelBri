// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    // Whether the wheel's own Windows software had a centring spring for force feedback games (Logitech Profiler and
    // Logitech Gaming Software for the G25 and G27: "Enable Centering Spring in Force Feedback Games" and "Centering
    // Spring Strength", 0 to 100%): the Mapping tab then offers it ([ffb] centring_spring, CentringSpring.hpp).
    bool centringSpring = false;

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
    // The combined pedals (combinedPedals) of the user's throttle and brake, set by encode: what the G25's and G27's
    // Slider 0 reports, and the throttle's axis with combined pedals on (withCombinedPedals). Carried as it is, so
    // moving the pedal fields never changes it.
    std::int32_t combined = 0;
    // The RGT's progressive levers, encoded as the pedals. After withRgtMode the RGT's four pedal axes are in
    // brake (Y), throttle (Rz), clutch (Slider 0) and rightLever (Slider 1); leftLever is then at rest.
    std::int32_t leftLever = 0;
    std::int32_t rightLever = 0;

    friend bool operator==(const TargetReport&, const TargetReport&) = default;
};

// The combined pedals axis of a target whose pedals rest at the top of the range (all four targets; Logitech's
// "Combined pedals"): the middle at rest, the throttle pulls it down and the brake pushes it up (both pressed: they
// cancel), as Race Driver: GRID's G25 preset reads Slider 0 (its lower half throttle, its upper half brake). 0..65535.
[[nodiscard]] constexpr std::int32_t combinedPedals(const TargetReport& report) {
    const std::int64_t value = (std::int64_t{65535} + report.throttle - report.brake) / 2;
    return static_cast<std::int32_t>(value < 0 ? 0 : value > 65535 ? 65535 : value);
}

// The middle of a pedal's range: where a pedal axis rests that carries no pedal (withCombinedPedals).
inline constexpr std::int32_t kPedalMiddle = 32767;

// `report` as Logitech's "Report Combined Pedals" sends it (Logitech Profiler, Logitech Gaming Software, G HUB; Linux
// hid-lg4ff's combine_pedals, whose code this follows): the throttle's axis carries the combined pedals, the brake's
// rests in the middle, the clutch is left as it is. For games that read throttle and brake from one axis (NFS
// Underground, Underground 2, Most Wanted), whose menus scroll by themselves with separate pedals.
[[nodiscard]] constexpr TargetReport withCombinedPedals(TargetReport report) {
    report.throttle = report.combined;
    report.brake = kPedalMiddle;
    return report;
}

// The order a target's buttons reach the game in ([buttons] order). Driver: as Logitech's Windows driver of their time
// (Logitech Profiler, Logitech Gaming Software) numbers the G25's and G27's, the order the Mapping tab names them in
// and what games of that time read (Race Driver: GRID's G25 preset). Native: as the wheel itself numbers them in HID,
// with no Logitech driver (Linux), which newer games' presets read (Assetto Corsa Competizione's G25 and G27 presets).
// Only a target that has a native order of its own (IDeviceProfile::hasNativeButtonOrder) offers it.
enum class ButtonOrder : std::uint8_t { Driver, Native };
[[nodiscard]] std::string_view toString(ButtonOrder order);           // "driver", "native"
[[nodiscard]] std::optional<ButtonOrder> parseButtonOrder(std::string_view text);

// A group of a target's buttons as the Mapping tab shows them ("FACE": A, B, X, Y), in a column of its own (Left,
// Right: groups stack down their column) or Below both columns (two buttons a row: the shifter's gears).
struct ButtonGroup {
    enum class Place { Left, Right, Below };
    std::string name;
    std::vector<std::size_t> buttons;   // target buttons (0-based), in the order shown
    Place place = Place::Left;

    friend bool operator==(const ButtonGroup&, const ButtonGroup&) = default;
};

// The buttons every target has and the Rig maps by itself (AutoMap.hpp): an Xbox-style rim's face buttons and the
// paddles. The G920 and G923 name them so; the G25 and G27 have no A, B, X, Y: an Xbox-style rim's go on the shifter's
// black diamond by place (A bottom, B right, X left, Y top), as games of their time used it for menus.
enum class CoreButton : std::uint8_t { A, B, X, Y, LPaddle, RPaddle };
inline constexpr std::size_t kCoreButtonCount = 6;
// What the Rig tab calls them ("It is", Rig.hpp's kButtonNames), in CoreButton's order.
inline constexpr std::array<std::string_view, kCoreButtonCount> kCoreButtonKinds = {"A", "B", "X", "Y", "L Paddle",
                                                                                   "R Paddle"};

// The H-pattern shifter's gears, which the Rig maps by itself too (AutoMap.hpp): what the Rig tab calls them ("It is"),
// Gear 1 .. Gear 7 then Reverse. A target with fewer gears has none for the rest (the Logitech shifters: 6 and R).
inline constexpr std::size_t kGearCount = 8;
inline constexpr std::size_t kReverseGear = kGearCount - 1;
inline constexpr std::array<std::string_view, kGearCount> kGearKinds = {"Gear 1", "Gear 2", "Gear 3", "Gear 4",
                                                                       "Gear 5", "Gear 6", "Gear 7", "Reverse"};

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
    // Whether its buttons have a native order besides the one they are named in (ButtonOrder::Native).
    [[nodiscard]] virtual bool hasNativeButtonOrder() const { return false; }
    // `buttons` (bit n = target button n, as named) in the native order: what the game gets with ButtonOrder::Native.
    // Unchanged for a target without one. No allocations (the 1 kHz loop calls it).
    [[nodiscard]] virtual std::uint32_t nativeButtons(const std::uint32_t buttons) const { return buttons; }
    // The target button (0-based, as named) of a core button, or -1 when it has none.
    [[nodiscard]] virtual int coreButton(CoreButton /*button*/) const { return -1; }
    // The target button (0-based, as named) of gear `gear` (kGearKinds' index), or -1 when it has none.
    [[nodiscard]] virtual int gearButton(std::size_t /*gear*/) const { return -1; }
};

// The number a game shows for target button `index` (0-based, as named), the way the real wheel numbers it there.
// ButtonOrder::Driver (and every target without a native order): Logitech's driver counts from 1, as joy.cpl and
// games' presets show it ("Button 7"), so index + 1. ButtonOrder::Native: the button's place in the wheel's own HID
// order, from 0, as it reads without Logitech's driver. -1 for an index past 31.
[[nodiscard]] int gameButtonNumber(const IDeviceProfile& profile, ButtonOrder order, std::size_t index);

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
    // The native order (ButtonOrder::Native): index = target button as named, value = its button in the wheel's own
    // HID order (0-based). Empty: none. Must be a permutation of 0..buttonNames.size() - 1.
    std::vector<std::uint8_t> nativeButtonOrder;
    // The target button of each CoreButton (IDeviceProfile::coreButton), in CoreButton's order; -1: none.
    std::array<int, kCoreButtonCount> coreButtons = {-1, -1, -1, -1, -1, -1};
    // The target button of each gear (IDeviceProfile::gearButton), in kGearKinds' order; -1: none.
    std::array<int, kGearCount> gearButtons = {-1, -1, -1, -1, -1, -1, -1, -1};
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
    [[nodiscard]] bool hasNativeButtonOrder() const override { return !spec_.nativeButtonOrder.empty(); }
    [[nodiscard]] std::uint32_t nativeButtons(std::uint32_t buttons) const override;
    [[nodiscard]] int coreButton(CoreButton button) const override;
    [[nodiscard]] int gearButton(std::size_t gear) const override;

private:
    [[nodiscard]] std::int32_t encodePedal(float travel) const;

    DeviceProfileSpec spec_;
    std::uint32_t buttonMask_ = 0;   // the named buttons: only they reach the game
    std::array<std::uint8_t, 32> native_{};   // spec_.nativeButtonOrder, for nativeButtons (identity past it)
};

// The G920's buttons with the Driving Force Shifter: 11 on the wheel, an unused 12th, then the shifter's gears 1-6 and
// reverse (Logitech's shifter FAQ: G920 buttons 13-18 and 19, 1-based), so 19 in all.
inline constexpr std::size_t kG920Buttons = 19;
inline constexpr std::size_t kG920FirstGear = 12;   // 0-based: Gear 1 .. Gear 6 are 12..17, Gear R is 18

// Logitech G920 Driving Force (Xbox One / PC) in PC mode. 0xc262 is the PID it enumerates with once G HUB
// or the kernel driver has switched it out of its Xbox-mode PID (0xc261). Product name, pedal encoding and
// button labels are unverified against a real G920. Its force feedback is HID++ (feature 0x8123, HidPpFfb.hpp) rather
// than the classic Logitech protocol.
[[nodiscard]] DeviceProfileSpec g920Spec();

// The G25's buttons (046D:C299): 19, the shifter's included (docs/target-g25.md), named in the order Logitech's Windows
// driver gives them (ButtonOrder::Driver): the shifter's red buttons 0..3 (left to right), paddles R 4 and L 5, the
// wheel's buttons R 6 and L 7, gears 1-6 and reverse 8..14, the shifter's black diamond 15..18 (top, left, bottom,
// right). Its native order (ButtonOrder::Native, the wheel's own HID order): black bottom, left, right, top 0..3,
// paddles and wheel buttons as above, red 2, 3, 4, 1 at 8..11, gears 12..17, reverse 18. Both from GIMX's G25 and G27
// configurations for Windows and for Linux (facts only, nothing copied: GPLv3) and a G27's HID descriptor; the
// diamond's native order matches Assetto Corsa Competizione's G25 preset (A, X, B, Y as on its G920 preset). Not
// checked on a real G25.
inline constexpr std::size_t kG25Buttons = 19;
inline constexpr std::size_t kG25FirstGear = 8;

// Logitech G25 Racing Wheel in native mode (046D:C299, after the mode switch a real one gets from Logitech's driver).
// The axes as Logitech's driver presents a G25 with separate pedals to games (Assetto Corsa Competizione's G25 preset:
// wheel X, throttle Y, brake Rz, clutch Slider 1; Race Driver: GRID's: combined pedals on Slider 0), released pedals at
// the top of the range; the virtual device and the shim present it so (VirtualWheel::combinedPedals, layoutFor). The
// 900 degrees are its full lock.
[[nodiscard]] DeviceProfileSpec g25Spec();

// The G27's buttons (046D:C29B): 23 (docs/target-g27.md), the G25's 19 in the same places and 4 more on the wheel,
// R 19, L 20, R 21, L 22 (top to bottom; which of each side's lower buttons is which is not confirmed). Native order:
// the G25's, except reverse 22 and the wheel's lower buttons R 18, 19, L 20, 21. From the same sources as the G25's.
inline constexpr std::size_t kG27Buttons = 23;
inline constexpr std::size_t kG27FirstGear = 8;

// Logitech G27 Racing Wheel in native mode (046D:C29B): presented like the G25 (the same axes, pedals released at the
// top, 900 degrees), with its 23 buttons. Its rev lights are not emulated (games drive them through Logitech's SDK).
[[nodiscard]] DeviceProfileSpec g27Spec();

// The G923 for Xbox and PC in PC mode (046D:C26E, docs/target-g923.md): 24 buttons, the G920's 19 (the same
// HID++ wheel family: Linux's hid-logitech-hidpp drives both with the G920's quirks) and the 5 the G920 lacks, after
// them: Plus 19, Minus 20, Dial Right 21, Dial Left 22, Dial Enter 23. Logitech does not document the order: the
// G920's part follows the G920, the last 5 the G29's order (+, -, dial right, dial left, enter); unverified until a
// real G923 is captured.
inline constexpr std::size_t kG923Buttons = 24;
inline constexpr std::size_t kG923FirstGear = kG920FirstGear;

// Logitech G923 Racing Wheel for Xbox One and PC in PC mode (046D:C26E; Xbox mode is C26D, which Logitech's driver or
// Linux's xpad switches out of): presented like the G920 (the same axes, pedals released at the top, 900 degrees),
// with its 24 buttons. Its rev lights are not emulated; TrueForce is (TrueForce.hpp).
[[nodiscard]] DeviceProfileSpec g923Spec();

// The RGT's buttons (Thrustmaster RGT Force Feedback Pro Clutch Edition, 044F:B653, docs/target-rgt.md): 10, as
// Thrustmaster's driver registers them (Buttons 0-9) and Race Driver: GRID's RGT preset reads them: 0 and 1 the
// paddles (gear down, gear up), 2-7 the rim's buttons, 8 and 9 the sequential stick (up a gear, down a gear).
inline constexpr std::size_t kRgtButtons = 10;
// Thrustmaster RGT Force Feedback Pro Clutch Edition (044F:B653), as games see it with Thrustmaster's driver: named
// "RGT Force Feedback Pro" (the name Thrustmaster's installer registers for B653 and Race Driver: GRID's preset 5
// reads), 270 degrees, pedals released at the top (255 at rest on a real one), 10 buttons, a D-pad. Which rim button
// is which number is not known yet: the names say Button 3..8.
[[nodiscard]] DeviceProfileSpec rgtSpec();

// The TMX's buttons (Thrustmaster TMX PRO Force Feedback, 044F:B67F, docs/target-tmx.md): 13, as the real wheel's
// HID report has them (captures/tmx). DiRT Rally 2.0's TMX preset reads 0 and 1 as the paddles (gear down, gear up);
// which printed rim button each other number is: not known yet.
inline constexpr std::size_t kTmxButtons = 13;
// Thrustmaster TMX PRO Force Feedback (the TMX with its three T3PA pedals; 044F:B67F), as games see it with
// Thrustmaster's driver: named "Thrustmaster TMX Racing Wheel" (the product string and the name Thrustmaster's
// installer registers for B67F), 900 degrees, pedals released at the top (1023 at rest on a real one), 13 buttons, a
// D-pad.
[[nodiscard]] DeviceProfileSpec tmxSpec();

// The RGT's axis mode and pedal set ([rgt] axes, pedal_set; Thrustmaster's manual, "Axis selection"): what its pedal
// axes carry. Combining follows from these, as on the real wheel; WheelBri's combined pedals do not apply to it.
enum class RgtAxes : std::uint8_t { Two = 2, Three = 3, Five = 5 };
enum class RgtPedalSet : std::uint8_t { Levers, Clutch };   // the pedal base's switch: 2 L, 3 P
struct RgtMode {
    RgtAxes axes = RgtAxes::Five;
    RgtPedalSet pedalSet = RgtPedalSet::Clutch;
    // As the real wheel: the clutch (3 P) always means 5 axes.
    [[nodiscard]] constexpr RgtAxes effectiveAxes() const {
        return pedalSet == RgtPedalSet::Clutch ? RgtAxes::Five : axes;
    }
    friend bool operator==(const RgtMode&, const RgtMode&) = default;
};

// `report` (the RGT's encode) with its pedal axes as the mode reports them (the manual's table):
//   2 axes: Y = gas (with the right lever) and brake (with the left lever) on one axis, as combinedPedals;
//   3 axes, levers: Y = brake with the left lever, Rz = gas with the right lever (the one pressed further);
//   5 axes, levers: Y brake, Rz gas, Slider 0 the left lever, Slider 1 the right lever;
//   5 axes, clutch: Y brake, Rz gas, Slider 0 the clutch, Slider 1 at rest.
// An axis the mode does not use rests at the top (65535), as the RGT's released pedals. Which way gas and brake move
// the 2 axes Y, and how 3 axes joins a pedal and a lever, are not measured on a real RGT yet.
[[nodiscard]] TargetReport withRgtMode(TargetReport report, RgtMode mode);

// Config names of the built-in profiles, for messages: the wheels WheelBri emulates (a virtual device and the shim both
// present them). A new target is a new profile here plus its support (driver, shim) first.
inline constexpr std::string_view kKnownProfiles = "g25, g27, g920, g923, rgt, tmx";

// Looks up a built-in profile by its config name ("g920", "g25", "g27", "g923", "rgt", "tmx"). Returns nullptr for
// unknown names.
[[nodiscard]] std::unique_ptr<IDeviceProfile> makeProfile(std::string_view name);


}  // namespace wheelbri
