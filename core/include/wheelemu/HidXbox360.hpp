#pragma once

#include "wheelemu/XboxPad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wheelemu {

// The HID side of the virtual Xbox 360 controller: what joy.cpl and DirectInput see, next to the XInput side
// (driver/xusb). A real wired Xbox 360 controller has both; "Controller (XBOX 360 For Windows)" in joy.cpl is its
// HID side. Served by the wheelemu-g920 driver under a second hardware ID (wheelemu\XBOXHID), fed by the service
// on its own port. The service keeps it idle: it is there to be listed, and DirectInput games would act on its
// buttons as well as on the XInput side's.
//
// The layout Windows gives a real one: left stick X/Y, right stick Rx/Ry, both triggers on one Z axis (left
// trigger up, right trigger down, centred when both or neither are pressed), 10 buttons (A, B, X, Y, LB, RB,
// Back, Start, left stick, right stick) and the D-pad as a hat. Its sticks and triggers never move: only buttons
// and the D-pad, as on the XInput side.
inline constexpr std::uint16_t kXboxHidVendorId = 0x045E;
inline constexpr std::uint16_t kXboxHidProductId = 0x028E;
inline constexpr std::uint16_t kXboxHidPort = 41333;   // service -> driver, the same "WEXP" packets as kXusbPort
inline constexpr std::uint8_t kXboxHidInputReportId = 0x01;
inline constexpr std::size_t kXboxHidInputReportSize = 14;   // report ID + X Y Rx Ry Z (u16 each) + buttons u16 + hat

[[nodiscard]] std::span<const std::uint8_t> xboxHidReportDescriptor();

// The input report for `pad` (XInput ranges, see XusbPad).
[[nodiscard]] std::array<std::uint8_t, kXboxHidInputReportSize> packXboxHidInput(const XusbPad& pad);

}  // namespace wheelemu
