#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidWheelDevice.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    TargetReport report = neutralReport();
    for (const std::int8_t hat : {std::int8_t{8}, std::int8_t{100}, std::int8_t{-5}, kHatCentered}) {
        report.hat = hat;
        EXPECT_EQ(packWheelInput(report)[12], 8) << static_cast<int>(hat);
    }
    report.hat = 7;
    EXPECT_EQ(packWheelInput(report)[12], 7);
}

// The descriptor for wheels the table may get later: HID++ with fewer than the G923's buttons has no HID++
// descriptor (the plain one for its buttons).
TEST(VirtualWheel, DescriptorsForOtherCombinations) {
    VirtualWheel small = *virtualWheel("g923");
    small.buttons = kG920Buttons;
    EXPECT_EQ(reportDescriptorFor(small).data(), wheelReportDescriptor(kG920Buttons).data());
}

// The descriptor's items as (prefix, value) pairs, long items skipped.
std::vector<std::pair<std::uint8_t, std::uint32_t>> itemsOf(const std::span<const std::uint8_t> d) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> out;
    for (std::size_t i = 0; i < d.size();) {
        const std::uint8_t prefix = d[i];
        const std::size_t size = (prefix & 3U) == 3U ? std::size_t{4} : std::size_t{prefix & 3U};
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < size; ++k) value |= std::uint32_t{d[i + 1 + k]} << (8 * k);
        out.emplace_back(prefix, value);
        i += 1 + size;
    }
    return out;
}

// The G25's and G27's classic identities: their own hardware ids and instances, the same profile, ids and names;
// found by hardware id and by request name, and only for those two.
TEST(VirtualWheel, TheG25AndG27HaveAClassicIdentity) {
    for (const char* name : {"g25", "g27"}) {
        const VirtualWheel& plain = *virtualWheel(name);
        const VirtualWheel* classic = classicVirtualWheel(name);
        ASSERT_NE(classic, nullptr) << name;
        EXPECT_TRUE(classic->classic);
        EXPECT_FALSE(plain.classic);
        EXPECT_EQ(classic->profile, plain.profile);
        EXPECT_EQ(classic->productId, plain.productId);
        EXPECT_EQ(classic->oemName, plain.oemName);
        EXPECT_EQ(classic->buttons, plain.buttons);
        EXPECT_NE(classic->hardwareId, plain.hardwareId);
        EXPECT_EQ(classic->instancePrefix, plain.instancePrefix);   // one device to games (their bindings)
        EXPECT_EQ(virtualWheelForHardwareId(classic->hardwareId), classic);
        EXPECT_EQ(virtualWheel(name), &plain);   // the profile still means the plain one
        EXPECT_EQ(virtualWheelForRequestName(requestNameOf(*classic)), classic);
        EXPECT_EQ(virtualWheelForRequestName(requestNameOf(plain)), &plain);
    }
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G25_LOGI"), classicVirtualWheel("g25"));
    EXPECT_EQ(requestNameOf(*classicVirtualWheel("g27")), "g27-logi");
    EXPECT_EQ(classicVirtualWheel("g920"), nullptr);
    EXPECT_EQ(classicVirtualWheel("g923"), nullptr);
    EXPECT_EQ(virtualWheelForRequestName("g920-logi"), nullptr);
    EXPECT_EQ(virtualWheelForRequestName("-logi"), nullptr);
}

// A classic descriptor: the plain one's input items without the Report ID, no HID PID, then one 7-byte vendor output
// report; its input report is the plain one's without the ID byte; every report it gets is Classic.
TEST(VirtualWheel, AClassicWheelHasNoReportIdsAndOneSevenByteOutput) {
    for (const char* name : {"g25", "g27"}) {
        const VirtualWheel& plain = *virtualWheel(name);
        const VirtualWheel& classic = *classicVirtualWheel(name);
        const auto items = itemsOf(reportDescriptorFor(classic));
        int outputs = 0;
        std::uint32_t page = 0;
        std::uint32_t size = 0;
        std::uint32_t count = 0;
        for (const auto& [prefix, value] : items) {
            EXPECT_NE(prefix, 0x85) << name;   // no Report ID
            if (prefix == 0x05 || prefix == 0x06) {
                page = value;
                EXPECT_NE(page, 0x0FU) << name;   // no HID PID
            }
            if (prefix == 0x75) size = value;
            if (prefix == 0x95) count = value;
            if (prefix == 0x91) {
                ++outputs;
                EXPECT_EQ(page, 0xFF00U) << name;
                EXPECT_EQ(size * count, 7U * 8U) << name;
            }
            EXPECT_NE(prefix, 0xB1) << name;   // no feature report
        }
        EXPECT_EQ(outputs, 1) << name;
        EXPECT_EQ(items.back().first, 0xC0) << name;
        // The input items are the plain descriptor's up to its PID block, less the Report ID.
        const auto plainItems = itemsOf(reportDescriptorFor(plain));
        std::vector<std::pair<std::uint8_t, std::uint32_t>> plainInput;
        for (const auto& item : plainItems) {
            if ((item.first == 0x05) && item.second == 0x0F) break;
            if (item.first != 0x85) plainInput.push_back(item);
        }
        ASSERT_LT(plainInput.size(), items.size()) << name;
        for (std::size_t i = 0; i < plainInput.size(); ++i) EXPECT_EQ(items[i], plainInput[i]) << name << " " << i;

        TargetReport report = neutralReport();
        report.steering = 12345;
        report.buttons = 0x5A5A5;
        report.hat = 3;
        const InputReport a = packInputFor(plain, report);
        const InputReport b = packInputFor(classic, report);
        ASSERT_EQ(b.size + 1, a.size) << name;
        EXPECT_EQ(a[0], kWheelInputReportId);
        for (std::size_t i = 0; i < b.size; ++i) EXPECT_EQ(b[i], a[i + 1]) << name << " " << i;

        EXPECT_EQ(routeReport(classic, 0).to, ReportRoute::To::Classic);
        EXPECT_EQ(routeReport(classic, 0x11).to, ReportRoute::To::Classic);
        EXPECT_EQ(routeReport(plain, 0x11).to, ReportRoute::To::Pid);
    }
}

}  // namespace
}  // namespace wheelbri
