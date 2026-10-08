#pragma once

#include "wheelbri/DeviceProfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wheelbri {

// The virtual G920 as a HID device (Phase 5, our own driver): its report descriptor and input report.
//
// Byte for byte the descriptor the bridge gave HIDMaestro, which NFS 2015 and Windows' pid.dll accepted
// with force feedback (the old HIDMaestro bridge, 2026-09-30): a joystick with wheel X, brake Y, clutch Rz and throttle
// Slider (16-bit, -32768..32767), 19 buttons (the wheel's 11, an unused 12th, the Driving Force Shifter's gears 1-6 and
// reverse; then 5 padding bits), a hat, and HIDMaestro's HID PID block (MIT) with
// one feature report (Create New Effect 0x11); Block Load 0x12, Pool 0x13 and State 0x14 are served as
// feature reads anyway (see HidPid). The input report has ID 1.
inline constexpr std::uint16_t kG920VendorId = 0x046D;
inline constexpr std::uint16_t kG920ProductId = 0xC262;
inline constexpr std::uint8_t kG920InputReportId = 0x01;
inline constexpr std::size_t kG920InputReportSize = 13;   // report ID + 4 axes x 2 + 3 button bytes + hat

[[nodiscard]] std::span<const std::uint8_t> g920ReportDescriptor();

// The same descriptor for Proton (wheelbri-uhid): the input report is unchanged, only two axes are renamed, clutch
// Rz -> Z and throttle Slider -> Rz. Proton's DirectInput has a hack for 046D:C262 made for the real G920's layout
// (dlls/dinput/joystick_hid.c: Z becomes Rz "Brake", Rz becomes a Slider "Clutch"); with the Windows descriptor our
// clutch landed in Slider 0, where NFS reads its throttle (2026-10-08). Through that hack this one gives the game
// what Windows gives it: wheel X, brake Y, clutch Rz, throttle Slider 0.
[[nodiscard]] std::span<const std::uint8_t> g920ProtonReportDescriptor();

// The input report for `report` (G920 ranges: steering 0..65535 centred at 32768, pedals 0..65535 with
// 65535 = released, buttons bit n, hat -1 or 0..7). The same packing as the bridge.
[[nodiscard]] std::array<std::uint8_t, kG920InputReportSize> packG920Input(const TargetReport& report);

// Centred wheel, released pedals, no buttons: what games see while no state arrives.
[[nodiscard]] TargetReport g920Neutral();

}  // namespace wheelbri
