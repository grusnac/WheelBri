// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidWheelDevice.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wheelbri {
namespace {

TEST(VirtualWheel, OneIdentityPerTarget) {
    ASSERT_EQ(virtualWheels().size(), 6U);   // G25, G27, G920, G923, RGT, TMX
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
    EXPECT_TRUE(isVirtualWheelId(0x044F, 0xB67F));    // the TMX in PC mode
    EXPECT_FALSE(isVirtualWheelId(0x044F, 0xB67E));   // a TMX in Xbox mode: a real one
    EXPECT_FALSE(isVirtualWheelId(0x044F, 0xB65D));   // a TMX between the two: a real one
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

// The TMX: four axes (X, brake Y, throttle Rz, clutch Slider; no Z) and 13 buttons, as DiRT Rally 2.0's TMX preset
// reads them; Thrustmaster's force feedback driver keeps its entry (makerPidDriver); a car controller for DirectInput.
TEST(VirtualWheel, TheTmxHasBrakeOnYThrottleOnRzAnd13Buttons) {
    const VirtualWheel* tmx = virtualWheel("tmx");
    ASSERT_NE(tmx, nullptr);
    EXPECT_EQ(tmx->hardwareId, "wheelbri\\TMX");
    EXPECT_EQ(tmx->buttons, kTmxButtons);
    EXPECT_TRUE(tmx->brakeOnY);
    EXPECT_TRUE(tmx->makerPidDriver);
    EXPECT_FALSE(tmx->hidpp);
    EXPECT_FALSE(tmx->combinedPedals);
    std::vector<std::uint32_t> usages;
    std::uint32_t usageMaximum = 0;
    for (const auto& [prefix, value] : itemsOf(reportDescriptorFor(*tmx))) {
        if (prefix == 0x09 && usages.size() < 8) usages.push_back(value);
        if (prefix == 0x29 && usageMaximum == 0) usageMaximum = value;   // the buttons' Usage Maximum
    }
    const std::vector<std::uint32_t> axes = {0x04, 0x30, 0x31, 0x35, 0x36};   // Joystick, X, Y, Rz, Slider
    ASSERT_GE(usages.size(), axes.size());
    EXPECT_TRUE(std::equal(axes.begin(), axes.end(), usages.begin()));
    EXPECT_EQ(usageMaximum, kTmxButtons);

    TargetReport report = neutralReport();
    report.throttle = 1000;
    report.brake = 60000;
    report.clutch = 30000;
    TargetReport swapped = report;
    swapped.throttle = report.brake;
    swapped.brake = report.throttle;
    const auto expected = packWheelInput(swapped, tmx->buttons);
    const InputReport packed = packInputFor(*tmx, report);
    ASSERT_EQ(packed.size, expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), packed.bytes.begin()));
    EXPECT_EQ(directInputOemData(*tmx)[0], kJoyHwsHasPov | kJoyHwsIsCarCtrl);
    EXPECT_EQ(directInputOemData(*tmx)[4], kTmxButtons);
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

// The RGT's maker's identity (rgt-tm, for Thrustmaster's own driver): its own hardware id and request name, the same
// profile, instances, ids and names; found by hardware id and by request name, and only for the RGT.
TEST(VirtualWheel, TheRgtHasAMakersIdentity) {
    const VirtualWheel& plain = *virtualWheel("tm_rgt");
    const VirtualWheel* maker = makerVirtualWheel("tm_rgt");
    ASSERT_NE(maker, nullptr);
    EXPECT_TRUE(maker->makerIdentity);
    EXPECT_FALSE(plain.makerIdentity);
    EXPECT_TRUE(maker->makerDriver);   // Thrustmaster's entries stay untouched for it too
    EXPECT_EQ(maker->profile, plain.profile);
    EXPECT_EQ(maker->vendorId, plain.vendorId);
    EXPECT_EQ(maker->productId, plain.productId);
    EXPECT_EQ(maker->oemName, plain.oemName);
    EXPECT_EQ(maker->buttons, plain.buttons);
    EXPECT_EQ(maker->instancePrefix, plain.instancePrefix);   // one device to games
    EXPECT_EQ(maker->hardwareId, "wheelbri\\TM_RGT_TM");
    EXPECT_EQ(virtualWheelForHardwareId("WHEELBRI\\tm_rgt_tm"), maker);
    EXPECT_EQ(virtualWheel("tm_rgt"), &plain);   // the profile still means the plain one
    EXPECT_EQ(requestNameOf(*maker), "tm_rgt-tm");
    EXPECT_EQ(virtualWheelForRequestName("tm_rgt-tm"), maker);
    EXPECT_EQ(virtualWheelForRequestName("tm_rgt"), &plain);
    EXPECT_EQ(makerVirtualWheel("g25"), nullptr);
    EXPECT_EQ(makerVirtualWheel("tmx"), nullptr);
    EXPECT_EQ(virtualWheelForRequestName("g920-tm"), nullptr);
    EXPECT_EQ(virtualWheelForRequestName("-tm"), nullptr);
}

// The maker's identity is the real RGT to the host: the captured descriptor byte for byte, the real input report 0x01
// (buttons, hat, X 10 bits, Y Rz Slider Dial a byte each), every output and feature report Maker (logged by the
// driver); the features a real one answered at plug-in answered the same, nothing else.
TEST(VirtualWheel, TheMakersIdentityIsTheRealRgt) {
    const VirtualWheel& maker = *makerVirtualWheel("tm_rgt");
    std::ifstream in(std::string(WHEELBRI_CONFIG_DIR) + "/../captures/rgt/b653-report-descriptor.hex");
    std::vector<std::uint8_t> captured;
    for (unsigned value = 0; in >> std::hex >> value;) captured.push_back(static_cast<std::uint8_t>(value));
    ASSERT_EQ(captured.size(), 455U);
    const auto descriptor = reportDescriptorFor(maker);
    EXPECT_TRUE(std::equal(descriptor.begin(), descriptor.end(), captured.begin(), captured.end()));

    for (const std::uint8_t id : {0x40, 0x41, 0x4A, 0x81, 0x85}) {
        EXPECT_EQ(routeReport(maker, id).to, ReportRoute::To::Maker) << int{id};
        EXPECT_EQ(routeReport(maker, id).id, id);
    }

    const auto fields = [&](const TargetReport& report) {
        const InputReport packed = packInputFor(maker, report);
        EXPECT_EQ(packed.size, 8U);
        EXPECT_EQ(packed[0], 0x01);
        std::uint64_t bits = 0;
        for (std::size_t i = 0; i < 7; ++i) bits |= static_cast<std::uint64_t>(packed[1 + i]) << (8 * i);
        struct Fields {
            unsigned buttons, hat;
            int x;
            unsigned y, rz, slider, dial;
        };
        const unsigned rawX = static_cast<unsigned>((bits >> 14) & 0x3FF);
        return Fields{static_cast<unsigned>(bits & 0x3FF), static_cast<unsigned>((bits >> 10) & 0xF),
                      rawX >= 512 ? static_cast<int>(rawX) - 1024 : static_cast<int>(rawX),
                      static_cast<unsigned>((bits >> 24) & 0xFF), static_cast<unsigned>((bits >> 32) & 0xFF),
                      static_cast<unsigned>((bits >> 40) & 0xFF), static_cast<unsigned>((bits >> 48) & 0xFF)};
    };
    TargetReport report;
    report.steering = 32768;   // centred
    report.brake = 65535;      // released: the top
    report.throttle = 0;       // pressed all the way
    report.clutch = 32768;
    report.rightLever = 65535;
    report.buttons = (1U << 0) | (1U << 9) | (1U << 10);   // a button past the tenth is not the RGT's
    report.hat = kHatCentered;
    auto f = fields(report);
    EXPECT_EQ(f.buttons, 0x201U);
    EXPECT_EQ(f.hat, 15U);   // released: out of 0..7
    EXPECT_EQ(f.x, 0);
    EXPECT_EQ(f.y, 255U);
    EXPECT_EQ(f.rz, 0U);
    EXPECT_EQ(f.slider, 128U);
    EXPECT_EQ(f.dial, 255U);
    report.steering = 0;
    report.hat = 2;   // right
    f = fields(report);
    EXPECT_EQ(f.x, -512);
    EXPECT_EQ(f.hat, 2U);
    report.steering = 65535;
    EXPECT_EQ(fields(report).x, 511);

    std::array<std::uint8_t, 8> out{};
    ASSERT_EQ(makerFeature(maker, 0x81, out), 5U);
    EXPECT_EQ(out[0], 0x81);
    EXPECT_EQ(out[1], 0x01);
    EXPECT_EQ(out[2], 0x21);
    EXPECT_EQ(out[4], 0xFF);
    EXPECT_EQ(makerFeature(maker, 0x82, out), 3U);
    EXPECT_EQ(out[1], 0x50);
    EXPECT_EQ(makerFeature(maker, 0x86, out), 3U);
    EXPECT_EQ(makerFeature(maker, 0x88, out), 3U);
    EXPECT_EQ(out[2], 0x02);
    EXPECT_EQ(makerFeature(maker, 0x85, out), 0U);   // not read at plug-in: unanswered
    EXPECT_EQ(makerFeature(maker, 0x81, std::span(out).first(4)), 0U);   // too small
    EXPECT_EQ(makerFeature(*virtualWheel("tm_rgt"), 0x81, out), 0U);   // the plain RGT has no such reports
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
