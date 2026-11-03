#include "wheelbri/LogitechFfbDriver.hpp"

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace wheelbri::logitech_ffb
