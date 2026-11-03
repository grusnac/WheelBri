#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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
// The virtual G25 (docs/targets.md): the same descriptor and report as the virtual G920, a native-mode G25's
// ids (046D:C299, never the Driving Force mode's C294).
inline constexpr std::uint16_t kG25VendorId = 0x046D;
inline constexpr std::uint16_t kG25ProductId = 0xC299;
// The virtual G27 (docs/targets.md): the same descriptor and report with 23 buttons, a native-mode G27's ids.
inline constexpr std::uint16_t kG27VendorId = 0x046D;
inline constexpr std::uint16_t kG27ProductId = 0xC29B;
// The virtual G923 for Xbox One and PC (docs/targets.md): the same descriptor and report with 24 buttons and
// no padding, the PC-mode ids (046D:C26E; Xbox mode, C26D, is an Xbox controller, not this).
inline constexpr std::uint16_t kG923VendorId = 0x046D;
inline constexpr std::uint16_t kG923ProductId = 0xC26E;
inline constexpr std::uint8_t kWheelInputReportId = 0x01;
inline constexpr std::size_t kWheelInputReportSize = 13;   // report ID + 4 axes x 2 + 3 button bytes + hat
// A wheel with combined pedals (VirtualWheel::combinedPedals) has a fifth axis (its clutch) after the fourth: 15 bytes.
inline constexpr std::size_t kMaxInputReportSize = kWheelInputReportSize + 2;

// The unique id ("uniq", EVIOCGUNIQ) wheelbri-uhid gives the Linux virtual wheel (any target). The service never
// lists a device with it as part of a rig: it would read its own output as input.
inline constexpr const char* kVirtualWheelUniq = "wheelbri-wheel";

// The wheels WheelBri's driver (wheelbri-wheel) can be: the descriptor and input report here for all of them, another
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
    // docs/targets.md). Its HID PID reports then sit kHidPpPidShift higher, out of their way.
    bool hidpp = false;
    std::uint8_t deviceType = 0;       // HID++ 0x0005's device type (0x0A: a steering wheel)
    // Whether its axes are unsigned (0..65535, centred at 32768), as a real Logitech wheel's: Logitech's software reads
    // them so (G HUB drew the virtual G923's signed wheel half a turn off, wrapping as it crossed the centre).
    // packInputFor packs them to match. The others keep HIDMaestro's signed axes (-32768..32767), as tested in games.
    bool unsignedAxes = false;
    // Whether it has the axes Logitech's driver gives a G25 or G27 with separate pedals (Windows and Linux): wheel X,
    // throttle Y, brake Rz, then two Sliders, the combined pedals (combinedPedals in DeviceProfile.hpp: in the middle
    // at rest, down with the throttle, up with the brake) on Slider 0 and the clutch on Slider 1 (a fifth axis; the
    // input report has the combined pedals where the others have the clutch, and the clutch after them). Race Driver:
    // GRID's G25 preset (ActionMap/actionMapPreset1.xml) reads Slider 0 as combined pedals: without it Slider 0 read 0,
    // the throttle held down; with the clutch there, at the top as Logitech's pedals rest, the brake held down (the car
    // reversed by itself). Assetto Corsa Competizione's G25 and G27 presets read the clutch on Slider 1 (on Linux too:
    // with the clutch on Slider 0 there, ACC under Proton had no clutch, 2026-10-26).
    bool combinedPedals = false;
    // Whether a real one has TrueForce, so the virtual one can have its interface (TrueForce.hpp): the G923.
    bool trueForce = false;
    // The G25's and G27's classic identity (classicVirtualWheel): as a real one, for Logitech's own force feedback
    // driver (G HUB's jerry_forcefeedback, LogitechFfbDriver.hpp), which writes Logitech's classic 7-byte commands
    // straight to the HID device. Its descriptor has no report IDs and no HID PID: the same input items as the
    // other identity, and one 7-byte vendor output report (page 0xFF00). The driver logs what arrives there.
    bool classic = false;
};
[[nodiscard]] std::span<const VirtualWheel> virtualWheels();
// By target_profile ("g920", "g25", "g27", "g923"), or by a hardware id (any case); null when it is none of them.
[[nodiscard]] const VirtualWheel* virtualWheel(std::string_view profile);
[[nodiscard]] const VirtualWheel* virtualWheelForHardwareId(std::string_view hardwareId);
// The classic identity of the G25 or G27 (VirtualWheel::classic: hardware id wheelbri\G25_LOGI, wheelbri\G27_LOGI,
// instances G25L_n, G27L_n; the same profile, ids and names); null for any other profile.
[[nodiscard]] const VirtualWheel* classicVirtualWheel(std::string_view profile);
// The name a CreateVirtualWheel request gives a wheel (HelperProtocol.hpp): its profile, or the classic identity's
// "g25-logi", "g27-logi"; and the wheel for such a name (null when it is none).
[[nodiscard]] std::string requestNameOf(const VirtualWheel& wheel);
[[nodiscard]] const VirtualWheel* virtualWheelForRequestName(std::string_view name);
// Whether vid:pid is one of the virtual wheels' ids (never hidden: games are meant to see it).
[[nodiscard]] bool isVirtualWheelId(std::uint16_t vendorId, std::uint16_t productId);

// `buttons`: the wheel's (VirtualWheel::buttons): the G920's and G25's 19, the G27's 23 or the G923's 24 (the same
// report; the button block declares 23 and 1 padding bit, or 24 and none).
[[nodiscard]] std::span<const std::uint8_t> wheelReportDescriptor(std::size_t buttons = kG920Buttons);

// The wheel's own descriptor: wheelReportDescriptor(buttons), and for a wheel with HID++ (VirtualWheel::hidpp) that
// with its HID PID report IDs kHidPpPidShift higher and two vendor collections after it (usage page 0xFF43: long
// report 0x11, 19 bytes each way; very long 0x12, 63 bytes; the reports a G920 has, Linux's hid-logitech-hidpp says);
// for a wheel with combined pedals (VirtualWheel::combinedPedals) the fifth axis. The same on Windows and Linux.
[[nodiscard]] std::span<const std::uint8_t> reportDescriptorFor(const VirtualWheel& wheel);

// No wheel has DirectInput axis entries (OEM\VID_xxxx&PID_xxxx\Axes) any more: until round 93 the G923 reported its
// brake on HID Z and clutch on Rz (a real one's layout, for G HUB) and had entries to move them back for games, but
// with JOY_HWS_ISCARCTRL DirectInput relabels a wheel with HID Z (Y as Slider, Z as Y) and the entries did not undo it:
// in ACC gas was the clutch, brake the throttle, clutch the brake (2026-10-21). Every wheel now reports the games'
// layout (throttle Y, brake Rz, clutch Slider; the G25 and G27 their two Sliders) and has no HID Z. The service
// removes the Axes keys it wrote before (the ones with its "WheelBri" value).

// DirectInput's OEMData for the wheel (OEM\VID_xxxx&PID_xxxx, 8 bytes: JOYREGHWSETTINGS' dwFlags, then dwNumButtons):
// JOY_HWS_HASPOV (0x02) and its buttons, plus JOY_HWS_ISCARCTRL (0x40) for all but a wheel with combinedPedals (the
// G25 and G27). With that flag DirectInput relabelled the axes of the virtual G25 when its clutch was HID Z: Race
// Driver: GRID got its HID Y (the throttle) as Slider 0 and HID Z (the clutch) as Y, so a released throttle was a
// brake held down and the car reversed (2026-10-19). Without it each axis keeps its own (the shim's layout, as tested
// in GRID; DirectInput then calls it a joystick, which GRID does not mind).
inline constexpr std::uint8_t kJoyHwsHasPov = 0x02;
inline constexpr std::uint8_t kJoyHwsIsCarCtrl = 0x40;
[[nodiscard]] std::array<std::uint8_t, 8> directInputOemData(const VirtualWheel& wheel);
inline constexpr std::uint8_t kHidPpPidShift = 0x10;

// Where a report the host sends goes, by its report ID: the wheel's HID PID (`id` as HidPid knows it), its HID++,
// or neither.
struct ReportRoute {
    enum class To { Pid, HidPp, Classic, None };   // Classic: any report of a classic wheel (`id` as sent)
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
// `buttons`: as for wheelReportDescriptor; the buttons past them are not sent.
[[nodiscard]] std::array<std::uint8_t, kWheelInputReportSize> packWheelInput(const TargetReport& report,
                                                                             std::size_t buttons = kG920Buttons);

// An input report of up to kMaxInputReportSize bytes: `size` of them used.
struct InputReport {
    std::array<std::uint8_t, kMaxInputReportSize> bytes{};
    std::size_t size = 0;
    [[nodiscard]] std::span<const std::uint8_t> view() const { return {bytes.data(), size}; }
    [[nodiscard]] std::uint8_t operator[](const std::size_t at) const { return bytes[at]; }
    friend bool operator==(const InputReport&, const InputReport&) = default;
};
// The input report for the wheel's own descriptor (reportDescriptorFor(wheel)): packWheelInput, with the axes
// unsigned when the wheel's are (VirtualWheel::unsignedAxes) and, for a wheel with combined pedals
// (VirtualWheel::combinedPedals) those as the fourth axis and the clutch as a fifth after it (15 bytes; else 13).
// A classic wheel's has no report ID: the same bytes without the first.
[[nodiscard]] InputReport packInputFor(const VirtualWheel& wheel, const TargetReport& report);

// Centred wheel, released pedals, no buttons: what games see while no state arrives.
[[nodiscard]] TargetReport neutralReport();

}  // namespace wheelbri
