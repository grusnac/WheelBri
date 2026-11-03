#include "wheelbri/LogitechFfbDriver.hpp"

#include <gtest/gtest.h>

namespace wheelbri::logitech_ffb {
namespace {

constexpr std::wstring_view kLgs = L"{00000000-1111-2222-3333-444444444444}";   // any other program's driver

TEST(LogitechFfbDriver, RemovesOtherDriversUnlessKept) {
    EXPECT_EQ(action(kGHubClsid, false, false), Action::Remove);
    EXPECT_EQ(action(kLgs, false, false), Action::Remove);
    EXPECT_EQ(action(kGHubClsid, false, true), Action::Leave);
    EXPECT_EQ(action(L"{62b43f0e-e7db-4329-8c13-a966d84a289f}", false, true), Action::Leave);   // any case
    EXPECT_EQ(action(kLgs, false, true), Action::Leave);
}

TEST(LogitechFfbDriver, LeavesNoEntryAndWindowsPidDriver) {
    for (const bool keep : {false, true}) {
        EXPECT_EQ(action(L"", false, keep), Action::Leave);
        EXPECT_EQ(action(kPidClsid, false, keep), Action::Leave);
        EXPECT_EQ(action(L"{eec6993a-b3fd-11d2-a916-00c04fb98638}", false, keep), Action::Leave);
    }
}

// WheelBri's own old copy of G HUB's entry is not G HUB's: it goes, kept or not, CLSID or none.
TEST(LogitechFfbDriver, AlwaysRemovesWheelBrisOldCopy) {
    for (const bool keep : {false, true}) {
        EXPECT_EQ(action(kGHubClsid, true, keep), Action::Remove);
        EXPECT_EQ(action(L"", true, keep), Action::Remove);
    }
}

}  // namespace
}  // namespace wheelbri::logitech_ffb
