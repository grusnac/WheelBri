#include "wheelemu/HidXbox360.hpp"

#include <algorithm>

namespace wheelemu {

namespace {

// Generic Desktop / Game Pad, one input report (ID 1):
//   X Y (left stick), Rx Ry (right stick), Z (triggers): 16 bits each, 0..65535, centred at 32768
//   10 buttons + 6 bits padding
//   hat: 4 bits, 1..8 clockwise from up (0 = centred, outside the range = null state) + 4 bits padding
constexpr std::uint8_t kDescriptor[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x05,                    // Usage (Game Pad)
    0xA1, 0x01,                    // Collection (Application)
    0x85, kXboxHidInputReportId,   //   Report ID (1)
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65535)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x02,                    //     Report Count (2)
    0x81, 0x02,                    //     Input (Data, Variable, Absolute)
    0xC0,                          //   End Collection
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x33,                    //     Usage (Rx)
    0x09, 0x34,                    //     Usage (Ry)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65535)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x02,                    //     Report Count (2)
    0x81, 0x02,                    //     Input (Data, Variable, Absolute)
    0xC0,                          //   End Collection
    0x09, 0x32,                    //   Usage (Z): the triggers
    0x15, 0x00,                    //   Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //   Logical Maximum (65535)
    0x75, 0x10,                    //   Report Size (16)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute)
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x01,                    //   Usage Minimum (1)
    0x29, 0x0A,                    //   Usage Maximum (10)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x0A,                    //   Report Count (10)
    0x81, 0x02,                    //   Input (Data, Variable, Absolute)
    0x75, 0x06,                    //   Report Size (6)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Constant): padding
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x09, 0x39,                    //   Usage (Hat Switch)
    0x15, 0x01,                    //   Logical Minimum (1)
    0x25, 0x08,                    //   Logical Maximum (8)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x46, 0x3B, 0x01,              //   Physical Maximum (315)
    0x65, 0x14,                    //   Unit (Degrees)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x42,                    //   Input (Data, Variable, Absolute, Null State)
    0x65, 0x00,                    //   Unit (None)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Constant): padding
    0xC0,                          // End Collection
};

void put16(std::array<std::uint8_t, kXboxHidInputReportSize>& report, const std::size_t at, const std::uint16_t value) {
    report[at] = static_cast<std::uint8_t>(value & 0xFFU);
    report[at + 1] = static_cast<std::uint8_t>(value >> 8U);
}

// XInput -32768..32767 -> HID 0..65535; `flip` for the Y axes (XInput up = positive, HID up = small).
std::uint16_t stick(const std::int16_t value, const bool flip) {
    const int v = flip ? -static_cast<int>(value) - 1 : static_cast<int>(value);
    return static_cast<std::uint16_t>(std::clamp(v + 32768, 0, 65535));
}

}  // namespace

std::span<const std::uint8_t> xboxHidReportDescriptor() { return kDescriptor; }

std::array<std::uint8_t, kXboxHidInputReportSize> packXboxHidInput(const XusbPad& pad) {
    std::array<std::uint8_t, kXboxHidInputReportSize> report{};
    report[0] = kXboxHidInputReportId;
    put16(report, 1, stick(pad.thumbLX, false));
    put16(report, 3, stick(pad.thumbLY, true));
    put16(report, 5, stick(pad.thumbRX, false));
    put16(report, 7, stick(pad.thumbRY, true));
    // Both triggers on Z, as Windows does for a real one: left trigger towards 65535, right towards 0.
    const int z = 32768 + (static_cast<int>(pad.leftTrigger) - static_cast<int>(pad.rightTrigger)) * 128;
    put16(report, 9, static_cast<std::uint16_t>(std::clamp(z, 0, 65535)));

    static constexpr std::array<std::uint16_t, 10> kButtons = {
        XusbPad::kA,     XusbPad::kB,    XusbPad::kX,         XusbPad::kY,         XusbPad::kLeftShoulder,
        XusbPad::kRightShoulder, XusbPad::kBack, XusbPad::kStart, XusbPad::kLeftThumb, XusbPad::kRightThumb};
    std::uint16_t buttons = 0;
    for (std::size_t i = 0; i < kButtons.size(); ++i) {
        if ((pad.buttons & kButtons[i]) != 0) buttons = static_cast<std::uint16_t>(buttons | (1U << i));
    }
    put16(report, 11, buttons);

    // D-pad -> hat 1..8 clockwise from up; 0 = centred (null). Opposite directions cancel.
    const int vertical = ((pad.buttons & XusbPad::kDpadUp) != 0 ? 1 : 0) - ((pad.buttons & XusbPad::kDpadDown) != 0 ? 1 : 0);
    const int horizontal =
        ((pad.buttons & XusbPad::kDpadRight) != 0 ? 1 : 0) - ((pad.buttons & XusbPad::kDpadLeft) != 0 ? 1 : 0);
    static constexpr std::uint8_t kHat[3][3] = {
        {6, 5, 4},   // down-left, down, down-right
        {7, 0, 3},   // left, centre, right
        {8, 1, 2},   // up-left, up, up-right
    };
    report[13] = kHat[vertical + 1][horizontal + 1];
    return report;
}

}  // namespace wheelemu
