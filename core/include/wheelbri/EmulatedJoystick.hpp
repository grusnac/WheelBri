// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The emulated wheel as DirectInput sees it: its objects (axes, buttons, POV), the game's data format,
// per-axis ranges, and how a TargetReport becomes the bytes GetDeviceState returns.
//
// Kept free of <dinput.h> so it can be unit-tested on any platform. The numeric constants below mirror
// DirectInput's; shim/src checks them against the real macros with static_asserts.

#include "wheelbri/DeviceProfile.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

namespace di {

// DIDFT_* object type bits.
inline constexpr std::uint32_t kAbsAxis = 0x00000002;
inline constexpr std::uint32_t kAxis = 0x00000003;          // DIDFT_ABSAXIS | DIDFT_RELAXIS
inline constexpr std::uint32_t kPushButton = 0x00000004;
inline constexpr std::uint32_t kButton = 0x0000000C;        // DIDFT_PSHBUTTON | DIDFT_TGLBUTTON
inline constexpr std::uint32_t kPov = 0x00000010;
inline constexpr std::uint32_t kAnyInstance = 0x00FFFF00;
inline constexpr std::uint32_t kInstanceMask = 0x00FFFF00;
inline constexpr std::uint32_t kFfActuator = 0x01000000;
inline constexpr std::uint32_t kOptional = 0x80000000;

// DIDOI_* object flags.
inline constexpr std::uint32_t kAspectMask = 0x00000F00;
inline constexpr std::uint32_t kAspectPosition = 0x00000100;
inline constexpr std::uint32_t kObjectFfActuator = 0x00000001;

inline constexpr std::uint8_t kButtonDown = 0x80;
inline constexpr std::uint32_t kPovCentered = 0xFFFFFFFF;

[[nodiscard]] constexpr std::uint32_t makeInstance(const std::uint32_t n) { return (n & 0xFFFF) << 8; }
[[nodiscard]] constexpr std::uint32_t instanceOf(const std::uint32_t type) { return (type >> 8) & 0xFFFF; }

}  // namespace di

// Which GUID a DirectInput object (or a data format entry) carries. `Any` = no GUID given.
// Slider1 is a layout's second Slider (DIJOYSTATE2's rglSlider[1]): its object carries Slider (GUID_Slider), as format
// entries for it do.
enum class ObjectGuid : std::uint8_t {
    Any, XAxis, YAxis, ZAxis, RxAxis, RyAxis, RzAxis, Slider, Button, Key, Pov, Other, Slider1
};

// What an object reports from the TargetReport.
// Combined: the throttle and the brake on one axis (combinedPedals in DeviceProfile.hpp), the G25's.
enum class Control : std::uint8_t { Steering, Throttle, Brake, Clutch, Combined, Button, Pov };

struct EmulatedObject {
    ObjectGuid guid = ObjectGuid::Other;
    Control control = Control::Button;
    std::uint32_t typeId = 0;           // DIDFT kind | instance (| DIDFT_FFACTUATOR): what EnumObjects reports
    std::uint32_t nativeOffset = 0;     // offset in DIJOYSTATE, used before the game sets a data format
    std::uint32_t buttonIndex = 0;      // Control::Button only
    std::string name;
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;

    [[nodiscard]] bool isAxis() const { return (typeId & di::kAxis) != 0; }
    [[nodiscard]] bool isButton() const { return (typeId & di::kButton) != 0; }
    [[nodiscard]] bool isPov() const { return (typeId & di::kPov) != 0; }
};

// One DIOBJECTDATAFORMAT of the game's data format.
struct FormatEntry {
    ObjectGuid guid = ObjectGuid::Any;
    std::uint32_t offset = 0;
    std::uint32_t type = 0;             // DIDFT_* filter: kind bits, instance (or any), optional flag
    std::uint32_t flags = 0;            // DIDOI_* (aspect)
};

// Which DirectInput axis carries each pedal (the wheel is always X), and how many buttons there are.
// The G920's, as a real one reports them on Windows: throttle Y, brake Rz, clutch Slider (what NFS Unbound reads,
// 2026-10-09). wheelbri-shim.ini can move a pedal to another axis.
struct JoystickLayout {
    ObjectGuid throttle = ObjectGuid::YAxis;
    ObjectGuid brake = ObjectGuid::RzAxis;
    ObjectGuid clutch = ObjectGuid::Slider;     // ObjectGuid::Any = the device has no clutch axis
    // The combined pedals' axis (Any: none). Left out when a pedal is on that axis (wheelbri-shim.ini, a game profile).
    ObjectGuid combined = ObjectGuid::Any;
    bool invertPedals = false;                  // flip pressed/released on top of the profile's encoding
    std::size_t buttonCount = 11;
};

// Built-in layout for a profile name: throttle Y, brake Rz, clutch Slider; the G25's and G27's combined pedals on
// Slider 0 and clutch on Slider 1 (Slider1), as their virtual devices (VirtualWheel::combinedPedals).
[[nodiscard]] JoystickLayout layoutFor(std::string_view profile, std::size_t buttonCount);
// False if two controls share an axis or a pedal uses X (the wheel).
[[nodiscard]] bool isValid(const JoystickLayout& layout);
// "x", "y", "z", "rx", "ry", "rz", "slider", "slider1"; "none" gives ObjectGuid::Any. nullopt for anything else.
[[nodiscard]] std::optional<ObjectGuid> parseAxisName(std::string_view name);
[[nodiscard]] std::string_view axisName(ObjectGuid guid);

struct LogicalRange {
    std::int32_t min = 0;
    std::int32_t max = 65535;
};

class EmulatedJoystick {
public:
    enum class FormatResult { Ok, InvalidParam };

    struct Change {
        std::uint32_t offset;           // in the game's data format
        std::uint32_t data;
    };

    // X = wheel (force feedback actuator), pedals on the layout's axes, buttons, one POV hat. Objects are
    // ordered (and numbered) by HID usage, as DirectInput does for a real HID wheel. Reports are in the
    // profile's 0..65535 range. The layout must satisfy isValid().
    explicit EmulatedJoystick(const JoystickLayout& layout);

    [[nodiscard]] const std::vector<EmulatedObject>& objects() const { return objects_; }
    [[nodiscard]] std::size_t axisCount() const;
    [[nodiscard]] std::size_t buttonCount() const;
    [[nodiscard]] std::size_t povCount() const;

    // Assigns format entries to objects the way DirectInput does: in order, each entry takes the first
    // unused object that matches its GUID, kind, instance and aspect. A non-optional entry that finds no
    // object, or an entry that does not fit in `dataSize`, makes the whole format invalid.
    FormatResult setDataFormat(const std::vector<FormatEntry>& entries, std::uint32_t dataSize);
    [[nodiscard]] bool hasDataFormat() const { return hasFormat_; }
    [[nodiscard]] std::uint32_t dataSize() const { return dataSize_; }

    // Object lookups used by GetProperty / SetProperty / GetObjectInfo.
    [[nodiscard]] std::optional<std::size_t> objectAtOffset(std::uint32_t offset) const;   // DIPH_BYOFFSET
    [[nodiscard]] std::optional<std::size_t> objectById(std::uint32_t typeId) const;       // DIPH_BYID
    [[nodiscard]] std::optional<std::size_t> objectByUsage(std::uint16_t page, std::uint16_t usage) const;
    [[nodiscard]] std::optional<std::uint32_t> formatOffset(std::size_t object) const;
    // Offset shown in EnumObjects / GetObjectInfo: the format offset if assigned, else the native one.
    [[nodiscard]] std::uint32_t reportedOffset(std::size_t object) const;

    void setRange(std::size_t object, LogicalRange range);
    [[nodiscard]] LogicalRange range(std::size_t object) const { return ranges_[object]; }

    // The value an object reports for `report`: scaled axis, 0x80/0 button, POV hundredths of a degree.
    [[nodiscard]] std::uint32_t valueOf(std::size_t object, const TargetReport& report) const;

    // Fills `out` (exactly dataSize() bytes) in the game's data format. Returns false on a size mismatch.
    bool writeState(const TargetReport& report, std::span<std::uint8_t> out) const;

    // What changed between two reports, for buffered input (GetDeviceData), in format order.
    [[nodiscard]] std::vector<Change> diff(const TargetReport& before, const TargetReport& after) const;

private:
    [[nodiscard]] static std::uint32_t sizeOf(const EmulatedObject& object);
    [[nodiscard]] bool matches(const FormatEntry& entry, const EmulatedObject& object) const;

    bool invertPedals_ = false;
    std::vector<EmulatedObject> objects_;
    std::vector<LogicalRange> ranges_;
    std::vector<std::optional<std::uint32_t>> assignedOffset_;   // per object
    std::vector<std::uint32_t> unassignedPovOffsets_;            // POV entries with no object: report centred
    std::uint32_t dataSize_ = 0;
    bool hasFormat_ = false;
};

}  // namespace wheelbri
