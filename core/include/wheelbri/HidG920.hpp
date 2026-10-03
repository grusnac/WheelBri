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
// Slider (16-bit, -32768..32767), 16 buttons (11 used), a hat, and HIDMaestro's HID PID block (MIT) with
// one feature report (Create New Effect 0x11); Block Load 0x12, Pool 0x13 and State 0x14 are served as
// feature reads anyway (see HidPid). The input report has ID 1.
inline constexpr std::uint16_t kG920VendorId = 0x046D;
inline constexpr std::uint16_t kG920ProductId = 0xC262;
inline constexpr std::uint8_t kG920InputReportId = 0x01;
inline constexpr std::size_t kG920InputReportSize = 12;   // report ID + 11 data bytes

[[nodiscard]] std::span<const std::uint8_t> g920ReportDescriptor();

// The input report for `report` (G920 ranges: steering 0..65535 centred at 32768, pedals 0..65535 with
// 65535 = released, buttons bit n, hat -1 or 0..7). The same packing as the bridge.
[[nodiscard]] std::array<std::uint8_t, kG920InputReportSize> packG920Input(const TargetReport& report);

// Centred wheel, released pedals, no buttons: what games see while no state arrives.
[[nodiscard]] TargetReport g920Neutral();

}  // namespace wheelbri
