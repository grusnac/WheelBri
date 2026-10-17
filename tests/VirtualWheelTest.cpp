#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidG920.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {
namespace {

TEST(VirtualWheel, OneIdentityPerTarget) {
    ASSERT_EQ(virtualWheels().size(), 4U);   // G25, G27, G920, G923
    for (const VirtualWheel& wheel : virtualWheels()) {
        const auto profile = makeProfile(wheel.profile);
        ASSERT_NE(profile, nullptr) << wheel.profile;
        // The same ids and button count as the target the shim presents.
        EXPECT_EQ(wheel.vendorId, profile->identity().vendorId);
        EXPECT_EQ(wheel.productId, profile->identity().productId);
        EXPECT_EQ(wheel.buttons, profile->buttonCount());
        EXPECT_EQ(wheel.oemName, profile->identity().productName);
    }
}

TEST(VirtualWheel, FoundByProfileAndHardwareId) {
    const VirtualWheel* g25 = virtualWheel("g25");
    ASSERT_NE(g25, nullptr);
    EXPECT_EQ(g25->productId, 0xC299);
    EXPECT_EQ(g25->hardwareId, "wheelbri\\G25");
    EXPECT_EQ(g25->instancePrefix, "G25");
    EXPECT_EQ(virtualWheelForHardwareId("WHEELBRI\\g25"), g25);   // Windows may change the case
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G920"), virtualWheel("g920"));
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G2"), nullptr);
    EXPECT_EQ(virtualWheel("g29"), nullptr);
    const VirtualWheel* g27 = virtualWheel("g27");
    ASSERT_NE(g27, nullptr);
    EXPECT_EQ(g27->productId, 0xC29B);
    EXPECT_EQ(g27->buttons, 23U);
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G27"), g27);
    const VirtualWheel* g923 = virtualWheel("g923");
    ASSERT_NE(g923, nullptr);
    EXPECT_EQ(g923->productId, 0xC26E);
    EXPECT_EQ(g923->buttons, 24U);
    EXPECT_EQ(g923->product, "G923 Racing Wheel for Xbox One and PC");
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G923"), g923);
}

TEST(VirtualWheel, ItsIdsAreKnown) {
    EXPECT_TRUE(isVirtualWheelId(0x046D, 0xC262));
    EXPECT_TRUE(isVirtualWheelId(0x046D, 0xC299));
    EXPECT_TRUE(isVirtualWheelId(0x046D, 0xC26E));
    EXPECT_FALSE(isVirtualWheelId(0x046D, 0xC26D));   // a G923 in Xbox mode: a real one
    EXPECT_FALSE(isVirtualWheelId(0x046D, 0xC294));   // a G25 in Driving Force mode: a real one
    EXPECT_FALSE(isVirtualWheelId(0x346E, 0x0005));
}

TEST(VirtualWheel, EveryKnownProfileIsAWheelAndBack) {
    // kKnownProfiles (for messages) names exactly the profiles makeProfile knows and the virtual wheels.
    std::vector<std::string> named;
    std::string_view rest = kKnownProfiles;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(", ");
        named.emplace_back(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 2);
    }
    ASSERT_EQ(named.size(), virtualWheels().size());
    for (std::size_t i = 0; i < named.size(); ++i) {
        EXPECT_NE(makeProfile(named[i]), nullptr) << named[i];
        EXPECT_EQ(virtualWheels()[i].profile, named[i]);
    }
}

TEST(VirtualWheel, IdentitiesAreDistinctAndFitWhereTheyGo) {
    const auto wheels = virtualWheels();
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        const VirtualWheel& wheel = wheels[i];
        EXPECT_LT(wheel.product.size(), 64U) << wheel.profile;     // the driver's product string buffer
        EXPECT_LT(wheel.oemName.size(), 128U) << wheel.profile;    // uhid's device name
        EXPECT_EQ(wheel.hardwareId.substr(0, 9), "wheelbri\\") << wheel.profile;
        EXPECT_EQ(virtualWheelForHardwareId(wheel.hardwareId), &wheel);
        for (std::size_t j = i + 1; j < wheels.size(); ++j) {
            EXPECT_NE(wheel.profile, wheels[j].profile);
            EXPECT_NE(wheel.hardwareId, wheels[j].hardwareId);
            EXPECT_NE(wheel.instancePrefix, wheels[j].instancePrefix);
            EXPECT_NE(wheel.productId, wheels[j].productId);
        }
    }
}

TEST(VirtualWheel, NothingElseIsAVirtualWheel) {
    EXPECT_EQ(virtualWheelForHardwareId(""), nullptr);
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G920 "), nullptr);   // no trimming: Windows gives it exactly
    EXPECT_EQ(virtualWheelForHardwareId("HID\\VID_046D&PID_C262"), nullptr);
    EXPECT_EQ(virtualWheel(""), nullptr);
    EXPECT_EQ(virtualWheel("G920"), nullptr);   // config names are lower case
    EXPECT_FALSE(isVirtualWheelId(0x046E, 0xC262));
    EXPECT_FALSE(isVirtualWheelId(0, 0));
}

// A hat value no wheel reports (8 and up, or below -1, from a corrupt report): the HID hat reads centred (8).
TEST(VirtualWheel, AHatOutOfRangeIsPackedCentred) {
    TargetReport report = g920Neutral();
    for (const std::int8_t hat : {std::int8_t{8}, std::int8_t{100}, std::int8_t{-5}, kHatCentered}) {
        report.hat = hat;
        EXPECT_EQ(packG920Input(report)[12], 8) << static_cast<int>(hat);
    }
    report.hat = 7;
    EXPECT_EQ(packG920Input(report)[12], 7);
}

// The descriptor for wheels the table may get later: HID++ with fewer than the G923's buttons has no HID++
// descriptor (the plain one for its buttons); one without Logitech's pedal layout gets the games' layout on Windows.
TEST(VirtualWheel, DescriptorsForOtherCombinations) {
    VirtualWheel small = *virtualWheel("g923");
    small.buttons = kG920Buttons;
    EXPECT_EQ(reportDescriptorFor(small, DescriptorFor::Windows).data(), g920ReportDescriptor(kG920Buttons).data());
    VirtualWheel gamesPedals = *virtualWheel("g923");
    gamesPedals.logitechPedals = false;
    EXPECT_EQ(reportDescriptorFor(gamesPedals, DescriptorFor::Windows).data(),
              reportDescriptorFor(*virtualWheel("g923"), DescriptorFor::Linux).data());
    EXPECT_NE(reportDescriptorFor(*virtualWheel("g923"), DescriptorFor::Windows).data(),
              reportDescriptorFor(*virtualWheel("g923"), DescriptorFor::Linux).data());
}

}  // namespace
}  // namespace wheelbri
