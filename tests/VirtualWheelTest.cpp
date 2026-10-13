#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidG920.hpp"

#include <gtest/gtest.h>

namespace wheelbri {
namespace {

TEST(VirtualWheel, OneIdentityPerTarget) {
    ASSERT_EQ(virtualWheels().size(), 2U);
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
}

TEST(VirtualWheel, ItsIdsAreKnown) {
    EXPECT_TRUE(isVirtualWheelId(0x046D, 0xC262));
    EXPECT_TRUE(isVirtualWheelId(0x046D, 0xC299));
    EXPECT_FALSE(isVirtualWheelId(0x046D, 0xC294));   // a G25 in Driving Force mode: a real one
    EXPECT_FALSE(isVirtualWheelId(0x346E, 0x0005));
}

}  // namespace
}  // namespace wheelbri
