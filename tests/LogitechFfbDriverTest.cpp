#include "wheelbri/LogitechFfbDriver.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wheelbri::logitech_ffb {
namespace {

constexpr std::wstring_view kLgs = L"{920B260D-BE85-4D30-8797-8BA0A23A5D53}";   // any other program's driver

// The G920 and G923 (HID++) keep another program's driver; the G25 and G27 do not.
TEST(LogitechFfbDriver, OnlyWheelsWithHidPpKeepOtherDrivers) {
    EXPECT_TRUE(keepsOtherDrivers(true));
    EXPECT_FALSE(keepsOtherDrivers(false));
}

TEST(LogitechFfbDriver, AnotherDriverStaysOnlyWhereKept) {
    for (const bool installed : {false, true}) {
        EXPECT_EQ(action(kGHubClsid, false, true, installed), Action::Leave);
        EXPECT_EQ(action(L"{62b43f0e-e7db-4329-8c13-a966d84a289f}", false, true, installed), Action::Leave);
        EXPECT_EQ(action(kGHubClsid, false, false, installed), Action::Remove);
        EXPECT_EQ(action(kLgs, false, false, installed), Action::Remove);
    }
}

// Windows' own entry goes only where it would hide another driver that stays; no entry is left as it is.
TEST(LogitechFfbDriver, WindowsEntryGoesOnlyToUncoverAnotherDriver) {
    EXPECT_EQ(action(kPidClsid, false, true, true), Action::Remove);
    EXPECT_EQ(action(L"{eec6993a-b3fd-11d2-a916-00c04fb98638}", false, true, true), Action::Remove);
    EXPECT_EQ(action(kPidClsid, false, true, false), Action::Leave);
    EXPECT_EQ(action(kPidClsid, false, false, true), Action::Leave);
    for (const bool keep : {false, true}) {
        EXPECT_EQ(action(L"", false, keep, true), Action::Leave);
    }
}

// WheelBri's own old copy goes, whatever it names.
TEST(LogitechFfbDriver, AlwaysRemovesWheelBrisOldCopy) {
    for (const bool keep : {false, true}) {
        EXPECT_EQ(action(kGHubClsid, true, keep, true), Action::Remove);
        EXPECT_EQ(action(L"", true, keep, false), Action::Remove);
    }
}

TEST(LogitechFfbDriver, OtherDriverIsNeitherWindowsNorWheelBris) {
    EXPECT_TRUE(isOtherDriver(kGHubClsid, false));
    EXPECT_TRUE(isOtherDriver(kLgs, false));
    EXPECT_FALSE(isOtherDriver(kGHubClsid, true));
    EXPECT_FALSE(isOtherDriver(kPidClsid, false));
    EXPECT_FALSE(isOtherDriver(L"", false));
}

template <std::size_t N>
std::string hexOf(const std::array<std::uint8_t, N>& data) {
    std::string text;
    for (const std::uint8_t b : data) {
        char two[3];
        std::snprintf(two, sizeof(two), "%02x", static_cast<unsigned>(b));
        text += two;
    }
    return text;
}

// The classic entry's values as G HUB's G923 entry has them (captures/g923/2026-11-01/S0-check/registry-HKLM-before.reg).
TEST(LogitechFfbDriver, TheClassicEntryIsGHubsG923Entry) {
    EXPECT_EQ(hexOf(bytes(std::array<std::uint32_t, 1>{kClassicCreatedBy})), "00080000");
    EXPECT_EQ(hexOf(bytes(kClassicAttributes)), "00000000e8030000e8030000");
    const char* const expected[] = {
        "0000000001860000ed030000ed03000030000000", "0100000002860000ef030000ef03000030000000",
        "0200000003860000ef030000ef03000030000000", "0300000003860000ef030000ef03000030000000",
        "0400000003860000ef030000ef03000030000000", "0500000003860000ef030000ef03000030000000",
        "0600000003860000ef030000ef03000030000000", "0700000004d800006d0300006d03000030000000",
        "0800000004d800006d0300006d03000030000000", "0900000004d800006d0300006d03000030000000",
        "0a00000004d800006d0300006d03000030000000",
    };
    for (std::size_t i = 0; i < kClassicEffects.size(); ++i) {
        EXPECT_EQ(hexOf(bytes(kClassicEffects[i].attributes)), expected[i]) << i;
    }
    EXPECT_EQ(hexOf(kWheelAxisAttributes), "01810000");
    EXPECT_EQ(hexOf(kWheelAxisFfAttributes), "0a00000000010000");
}

TEST(LogitechFfbDriver, AClassForTheG25AndG27Only) {
    EXPECT_EQ(classicClsid(0xC299), kG25Clsid);
    EXPECT_EQ(classicClsid(0xC29B), kG27Clsid);
    EXPECT_TRUE(classicClsid(0xC262).empty());
    EXPECT_TRUE(classicClsid(0xC26E).empty());
}

}  // namespace
}  // namespace wheelbri::logitech_ffb
