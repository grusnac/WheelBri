#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace wheelbri {

// The virtual G920 as a HID device (Phase 5, our own driver): its report descriptor and input report.
//
// Byte for byte the descriptor the bridge gave HIDMaestro, which NFS 2015 and Windows' pid.dll accepted
// with force feedback (the old HIDMaestro bridge, 2026-09-30): a joystick with four 16-bit axes (-32768..32767) X, Y,
// Rz and Slider, which carry the wheel, throttle, brake and clutch as a real G920 does on Windows (NFS Unbound reads
// them so, 2026-10-09; up to 0.9.2 they carried wheel, brake, clutch and throttle), 19 buttons (the wheel's 11, an
// unused 12th, the Driving Force Shifter's gears 1-6 and reverse; then 5 padding bits), a hat, and HIDMaestro's HID
// PID block (MIT) with
// one feature report (Create New Effect 0x11); Block Load 0x12, Pool 0x13 and State 0x14 are served as
// feature reads anyway (see HidPid). The input report has ID 1.
inline constexpr std::uint16_t kG920VendorId = 0x046D;
inline constexpr std::uint16_t kG920ProductId = 0xC262;
// The virtual G25 (README, "Targets"): the same descriptor and report as the virtual G920, a native-mode G25's
// ids (046D:C299, never the Driving Force mode's C294).
inline constexpr std::uint16_t kG25VendorId = 0x046D;
inline constexpr std::uint16_t kG25ProductId = 0xC299;
// The virtual G27 (README, "Targets"): the same descriptor and report with 23 buttons, a native-mode G27's ids.
inline constexpr std::uint16_t kG27VendorId = 0x046D;
inline constexpr std::uint16_t kG27ProductId = 0xC29B;
// The virtual G923 for Xbox One and PC (README, "Targets"): the same descriptor and report with 24 buttons and
// no padding, the PC-mode ids (046D:C26E; Xbox mode, C26D, is an Xbox controller, not this).
inline constexpr std::uint16_t kG923VendorId = 0x046D;
inline constexpr std::uint16_t kG923ProductId = 0xC26E;
inline constexpr std::uint8_t kG920InputReportId = 0x01;
inline constexpr std::size_t kG920InputReportSize = 13;   // report ID + 4 axes x 2 + 3 button bytes + hat

// The unique id ("uniq", EVIOCGUNIQ) wheelbri-uhid gives the Linux virtual wheel (any target). The service never
// lists a device with it as part of a rig: it would read its own output as input.
inline constexpr const char* kG920VirtualUniq = "wheelbri-g920";

// The wheels WheelBri's driver (wheelbri-g920) can be: the descriptor and input report here for all of them, another
// identity each. The service creates the software device with the identity's hardware id (wheelbri\G920,
// wheelbri\G25); the driver, bound to every one of them by its INF, answers with that identity's ids and names.
struct VirtualWheel {
    std::string_view profile;          // target_profile: "g920", "g25", "g27", "g923"
    std::string_view hardwareId;       // the software device's hardware id: wheelbri\G920
    std::string_view instancePrefix;   // its instance ids: G920_0 .. G920_3 (spares for a stuck one)
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string_view product;          // the HID product string: "G920 Driving Force Racing Wheel USB"
    std::string_view oemName;          // DirectInput's name for it (OEMName): "Logitech G920 Driving Force Racing ..."
    std::size_t buttons = 0;           // DirectInput's OEMData button count
    // Whether it also has Logitech's HID++ reports (long 0x11, very long 0x12) for G HUB (HidPp.hpp,
    // README, "G HUB and HID++"). Its HID PID reports then sit kHidPpPidShift higher, out of their way.
    bool hidpp = false;
    std::uint8_t deviceType = 0;       // HID++ 0x0005's device type (0x0A: a steering wheel)
    // Whether its axes are unsigned (0..65535, centred at 32768), as a real Logitech wheel's: Logitech's software reads
    // them so (G HUB drew the virtual G923's signed wheel half a turn off, wrapping as it crossed the centre).
    // packInputFor packs them to match. The others keep HIDMaestro's signed axes (-32768..32767), as tested in games.
    bool unsignedAxes = false;
    // Whether its pedals are reported in HID as a real Logitech wheel's are (brake Z, clutch Rz), for Logitech's
    // software, which reads them by HID usage (G HUB took the brake on Rz for the clutch). Games keep reading them
    // where they always did (brake Rz, clutch Slider) through DirectInput's axis entries for the wheel
    // (directInputAxisEntries), which the service writes while the wheel exists. Windows only: wheelbri-uhid keeps the
    // games' layout (Proton has no such entries).
    bool logitechPedals = false;
};
[[nodiscard]] std::span<const VirtualWheel> virtualWheels();
// By target_profile ("g920", "g25", "g27", "g923"), or by a hardware id (any case); null when it is none of them.
[[nodiscard]] const VirtualWheel* virtualWheel(std::string_view profile);
[[nodiscard]] const VirtualWheel* virtualWheelForHardwareId(std::string_view hardwareId);
// Whether vid:pid is one of the virtual wheels' ids (never hidden: games are meant to see it).
[[nodiscard]] bool isVirtualWheelId(std::uint16_t vendorId, std::uint16_t productId);

// `buttons`: the wheel's (VirtualWheel::buttons): the G920's and G25's 19, the G27's 23 or the G923's 24 (the same
// report; the button block declares 23 and 1 padding bit, or 24 and none).
[[nodiscard]] std::span<const std::uint8_t> g920ReportDescriptor(std::size_t buttons = kG920Buttons);

// The wheel's own descriptor: g920ReportDescriptor(buttons), and for a wheel with HID++ (VirtualWheel::hidpp) that
// with its HID PID report IDs kHidPpPidShift higher and two vendor collections after it (usage page 0xFF43: long
// report 0x11, 19 bytes each way; very long 0x12, 63 bytes; the reports a G920 has, Linux's hid-logitech-hidpp says).
enum class DescriptorFor { Windows, Linux };
[[nodiscard]] std::span<const std::uint8_t> reportDescriptorFor(const VirtualWheel& wheel,
                                                              DescriptorFor where = DescriptorFor::Windows);

// DirectInput's per-device axis entries for a wheel with logitechPedals: OEM\VID_xxxx&PID_xxxx\Axes\<instance>,
// value "Attributes" = DIOBJECTATTRIBUTES (dwFlags, wUsagePage, wUsage; 8 bytes, little-endian): the DirectInput axis
// <instance> (0 X, 1 Y, 2 Z, 5 Rz, 6 Slider) is fed by that HID usage. Rz <- HID Z (the brake), Slider <- HID Rz (the
// clutch), and Z <- HID Slider, which the wheel does not have, so the brake is not also seen on Z. Empty for others.
struct DirectInputAxisEntry {
    int instance = 0;
    std::array<std::uint8_t, 8> attributes{};
};
[[nodiscard]] std::vector<DirectInputAxisEntry> directInputAxisEntries(const VirtualWheel& wheel);
inline constexpr std::uint8_t kHidPpPidShift = 0x10;

// Where a report the host sends goes, by its report ID: the wheel's HID PID (`id` as HidPid knows it), its HID++,
// or neither.
struct ReportRoute {
    enum class To { Pid, HidPp, None };
    To to = To::None;
    std::uint8_t id = 0;
};
[[nodiscard]] ReportRoute routeReport(const VirtualWheel& wheel, std::uint8_t reportId);
// The report ID on the wire for HidPid's `pidId` (a feature report the device answers).
[[nodiscard]] std::uint8_t wirePidId(const VirtualWheel& wheel, std::uint8_t pidId);

// The same descriptor for Proton (wheelbri-uhid): the input report is unchanged, only two axes are renamed, brake
// Rz -> Z and clutch Slider -> Rz, which is the real G920's own HID layout. Proton's DirectInput has a hack for
// 046D:C262 made for that layout (dlls/dinput/joystick_hid.c: Z becomes Rz "Brake", Rz becomes a Slider "Clutch");
// with the Windows descriptor the axes would shift (2026-10-08). Through that hack this one gives the game what
// Windows gives it: wheel X, throttle Y, brake Rz, clutch Slider 0.
[[nodiscard]] std::span<const std::uint8_t> g920ProtonReportDescriptor();

// The input report for `report` (G920 ranges: steering 0..65535 centred at 32768, pedals 0..65535 with
// 65535 = released, buttons bit n, hat -1 or 0..7): wheel X, throttle Y, brake Rz, clutch Slider.
// `buttons`: as for g920ReportDescriptor; the buttons past them are not sent.
[[nodiscard]] std::array<std::uint8_t, kG920InputReportSize> packG920Input(const TargetReport& report,
                                                                          std::size_t buttons = kG920Buttons);

// The input report for the wheel's own descriptor (reportDescriptorFor): packG920Input, with the axes unsigned when
// the wheel's are (VirtualWheel::unsignedAxes).
[[nodiscard]] std::array<std::uint8_t, kG920InputReportSize> packInputFor(const VirtualWheel& wheel,
                                                                          const TargetReport& report);

// Centred wheel, released pedals, no buttons: what games see while no state arrives.
[[nodiscard]] TargetReport g920Neutral();

}  // namespace wheelbri
