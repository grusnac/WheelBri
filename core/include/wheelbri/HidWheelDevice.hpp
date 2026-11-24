// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// The virtual wheels as a HID device (the wheelbri-wheel driver, wheelbri-uhid): report descriptor and input report.
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
// The virtual G25 (docs/target-g25.md): the same descriptor and report as the virtual G920, a native-mode G25's
// ids (046D:C299, never the Driving Force mode's C294).
inline constexpr std::uint16_t kG25VendorId = 0x046D;
inline constexpr std::uint16_t kG25ProductId = 0xC299;
// The virtual G27 (docs/target-g27.md): the same descriptor and report with 23 buttons, a native-mode G27's ids.
inline constexpr std::uint16_t kG27VendorId = 0x046D;
inline constexpr std::uint16_t kG27ProductId = 0xC29B;
// The virtual G923 for Xbox One and PC (docs/target-g923.md): the same descriptor and report with 24 buttons and
// no padding, the PC-mode ids (046D:C26E; Xbox mode, C26D, is an Xbox controller, not this).
inline constexpr std::uint16_t kG923VendorId = 0x046D;
inline constexpr std::uint16_t kG923ProductId = 0xC26E;
// The virtual RGT (Thrustmaster RGT Force Feedback Pro Clutch Edition, docs/target-rgt.md): a real one's ids
// (044F:B653) and product string, the same HID PID block, its 10 buttons, and five axes as games read the real one in
// its 5-axis mode (VirtualWheel::rgtAxes). Not the real wheel's own descriptor (a cut-down HID PID set that Windows'
// driver cannot drive, tests/data/rgt): WheelBri's, so force feedback needs no Thrustmaster software.
inline constexpr std::uint16_t kRgtVendorId = 0x044F;
inline constexpr std::uint16_t kRgtProductId = 0xB653;
// The virtual TMX (Thrustmaster TMX PRO Force Feedback, docs/target-tmx.md): a real one's ids (044F:B67F, the PC mode
// its Xbox mode, B67E, is switched out of) and product string, WheelBri's HID PID block, its 13 buttons and four axes
// as games read it (VirtualWheel::brakeOnY). On a real PC Thrustmaster's driver (tmhidusb.sys) also shows Windows a
// HID PID wheel (captures/tmx), so force feedback needs no Thrustmaster software.
inline constexpr std::uint16_t kTmxVendorId = 0x044F;
inline constexpr std::uint16_t kTmxProductId = 0xB67F;
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
    // docs/target-g923.md). Its HID PID reports then sit kHidPpPidShift higher, out of their way.
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
    // reversed by itself). Assetto Corsa Competizione's G25 and G27 presets read the clutch on Slider 1 (on Linux too;
    // there the clutch is Usage Dial, protonReportDescriptor).
    bool combinedPedals = false;
    // Whether a real one has TrueForce, so the virtual one can have its interface (TrueForce.hpp): the G923.
    bool trueForce = false;
    // The G25's and G27's classic identity (classicVirtualWheel): as a real one, for Logitech's own force feedback
    // driver (G HUB's jerry_forcefeedback, LogitechFfbDriver.hpp), which writes Logitech's classic 7-byte commands
    // straight to the HID device. Its descriptor has no report IDs and no HID PID: the same input items as the
    // other identity, and one 7-byte vendor output report (page 0xFF00). The driver logs what arrives there.
    bool classic = false;
    // The RGT's axes (Thrustmaster's 5-axis mode, the pedal switch on 3 P): wheel X, brake Y, throttle Rz, clutch
    // Slider 0, the right lever Slider 1 (at rest), as Race Driver: GRID's RGT preset reads them. The same five axes
    // as combinedPedals, carrying other things.
    bool rgtAxes = false;
    // Throttle Rz and brake Y (the G920's the other way round), the clutch on Slider: the TMX, as DiRT Rally 2.0's TMX
    // preset reads it (captures/tmx).
    bool brakeOnY = false;
    // Whether the maker's own DirectInput force feedback driver speaks HID PID, so its OEMForceFeedback entry stays
    // and drives the virtual wheel when installed (LogitechFfbDriver.hpp's keepsOtherDrivers): the TMX, Thrustmaster's
    // tmpid.dll (captures/tmx). Whether it does so without Thrustmaster's kernel driver: not tested yet.
    bool makerPidDriver = false;
    // Whether the maker's own force feedback driver, which does not speak HID PID, keeps its OEMForceFeedback entry and
    // drives the virtual wheel when installed: the RGT, Thrustmaster's tmffbdrv.dll (rgt-clutch-plan.md); WheelBri
    // does not read what it sends yet. The maker's installer also writes the same OEMName WheelBri does, so WheelBri
    // marks the values it writes in that OEM key ("WheelBri") and never writes over or removes the maker's
    // (writeDirectInputEntries, the uninstaller's oemEntries, clean-pc.ps1).
    bool makerDriver = false;
    // The maker's identity (makerVirtualWheel: the RGT's "rgt-tm"): as the real wheel, for the maker's own force
    // feedback driver, which talks the real wheel's protocol: its report descriptor as captured (tests/data/rgt), its
    // input report, and every output and feature report logged by the driver as it comes (not decoded yet); the
    // feature reports a real one answered at plug-in are answered the same (makerFeature).
    bool makerIdentity = false;
};
[[nodiscard]] std::span<const VirtualWheel> virtualWheels();
// By target_profile ("g920", "g25", "g27", "g923"), or by a hardware id (any case); null when it is none of them.
[[nodiscard]] const VirtualWheel* virtualWheel(std::string_view profile);
[[nodiscard]] const VirtualWheel* virtualWheelForHardwareId(std::string_view hardwareId);
// The classic identity of the G25 or G27 (VirtualWheel::classic: hardware id wheelbri\G25_LOGI, wheelbri\G27_LOGI;
// the same profile, instances, ids and names); null for any other profile.
[[nodiscard]] const VirtualWheel* classicVirtualWheel(std::string_view profile);
// The maker's identity of the RGT (VirtualWheel::makerIdentity: hardware id wheelbri\TM_RGT_TM, request name
// "tm_rgt-tm"; the same profile, instances, ids and names), used when Thrustmaster's driver is installed
// (makerFfbDriverInstalled); null for any other profile.
[[nodiscard]] const VirtualWheel* makerVirtualWheel(std::string_view profile);
// A feature report the maker's identity answers as the real wheel did at plug-in (tests/data/rgt: 0x81 01 21 ff ff,
// 0x82 50 5a, 0x86 00 01, 0x88 00 02): [report ID, data...] into `out`; its size, 0 for any other report (or wheel)
// or when `out` is too small.
[[nodiscard]] std::size_t makerFeature(const VirtualWheel& wheel, std::uint8_t reportId, std::span<std::uint8_t> out);

// The maker's identity's feature reports as a real device keeps them: a read gives what was last written to that report
// (Thrustmaster's driver writes 0x81 and then reads it back, again and again: 2026-11-23, wheelbri-driver-*-27), the
// plug-in value (makerFeature) until then. Up to kMaxBytes of a report kept. Thread-safe (the driver's queue is
// parallel).
class MakerFeatures {
public:
    static constexpr std::size_t kMaxBytes = 8;
    void written(std::uint8_t reportId, std::span<const std::uint8_t> data);
    // [report ID, data...] into `out`: its size; 0 for a wheel that is not a maker's identity, a report neither written
    // nor answered at plug-in, or an `out` too small.
    [[nodiscard]] std::size_t read(const VirtualWheel& wheel, std::uint8_t reportId, std::span<std::uint8_t> out) const;

private:
    struct Kept {
        std::array<std::uint8_t, kMaxBytes> data{};
        std::size_t size = 0;
        bool written = false;
    };
    mutable std::mutex lock_;
    std::array<Kept, 256> kept_{};
};
// The name a CreateVirtualWheel request gives a wheel (HelperProtocol.hpp): its profile, or the classic identity's
// "g25-logi", "g27-logi", or the maker's identity's "tm_rgt-tm"; and the wheel for such a name (null when it is none).
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
    // Classic: any report of a classic wheel; Maker: any output or feature report of a maker's identity (`id` as sent).
    enum class To { Pid, HidPp, Classic, Maker, None };
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

// The descriptor wheelbri-uhid gives Proton: the G920's g920ProtonReportDescriptor; the G25's and G27's
// reportDescriptorFor with the clutch's Usage Dial instead of the second Slider (same report bytes). Wine's dinput
// reads each axis by its usage (HidP_GetUsageValue), so with two Sliders both read the first: in ACC the clutch was
// the combined pedals, at half at rest (2026-11-07). Wine makes Slider and Dial Slider 0 and Slider 1 in the
// descriptor's order, as Windows does the two Sliders. The RGT's likewise (its right lever, Slider 1, as Dial: the real
// RGT's own descriptor has Slider and Dial). Every other wheel: reportDescriptorFor.
[[nodiscard]] std::span<const std::uint8_t> protonReportDescriptor(const VirtualWheel& wheel);

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
