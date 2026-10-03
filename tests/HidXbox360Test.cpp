#include "wheelbri/HidXbox360.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

std::uint16_t at16(const std::array<std::uint8_t, kXboxHidInputReportSize>& r, const std::size_t i) {
    return static_cast<std::uint16_t>(r[i] | (r[i + 1] << 8U));
}

TEST(HidXbox360, IdleIsCentredWithNothingPressed) {
    const auto report = packXboxHidInput(XusbPad{});
    EXPECT_EQ(report[0], kXboxHidInputReportId);
    EXPECT_EQ(at16(report, 1), 32768);   // X
    EXPECT_EQ(at16(report, 3), 32767);   // Y (flipped: XInput 0 -> just below the centre, as Windows does)
    EXPECT_EQ(at16(report, 9), 32768);   // Z: no trigger
    EXPECT_EQ(at16(report, 11), 0);      // buttons
    EXPECT_EQ(report[13], 0);            // hat centred
}

TEST(HidXbox360, ButtonsInWindowsOrder) {
    XusbPad pad;
    pad.buttons = XusbPad::kA | XusbPad::kY | XusbPad::kStart | XusbPad::kRightThumb;
    const auto report = packXboxHidInput(pad);
    EXPECT_EQ(at16(report, 11), (1U << 0) | (1U << 3) | (1U << 7) | (1U << 9));
}

TEST(HidXbox360, DpadIsAHat) {
    XusbPad pad;
    pad.buttons = XusbPad::kDpadUp;
    EXPECT_EQ(packXboxHidInput(pad)[13], 1);
    pad.buttons = XusbPad::kDpadRight | XusbPad::kDpadDown;
    EXPECT_EQ(packXboxHidInput(pad)[13], 4);
    pad.buttons = XusbPad::kDpadLeft;
    EXPECT_EQ(packXboxHidInput(pad)[13], 7);
    pad.buttons = XusbPad::kDpadUp | XusbPad::kDpadDown;   // opposite: cancel
    EXPECT_EQ(packXboxHidInput(pad)[13], 0);
}

TEST(HidXbox360, SticksAndTriggers) {
    XusbPad pad;
    pad.thumbLX = 32767;
    pad.thumbLY = 32767;   // up
    pad.leftTrigger = 255;
    const auto report = packXboxHidInput(pad);
    EXPECT_EQ(at16(report, 1), 65535);
    EXPECT_EQ(at16(report, 3), 0);
    EXPECT_EQ(at16(report, 9), 65408);
}

TEST(HidXbox360, DescriptorDescribesTheReport) {
    const auto descriptor = xboxHidReportDescriptor();
    ASSERT_GT(descriptor.size(), 20U);
    EXPECT_EQ(descriptor[0], 0x05);   // Usage Page (Generic Desktop)
    EXPECT_EQ(descriptor[3], 0x05);   // Usage (Game Pad)
    EXPECT_EQ(descriptor.back(), 0xC0);
    // Input bits: 5 axes x 16 + 10 buttons + 6 pad + 4 hat + 4 pad = 104 bits = 13 bytes after the report ID.
    EXPECT_EQ(kXboxHidInputReportSize, 1U + 13U);
}

}  // namespace
}  // namespace wheelbri
