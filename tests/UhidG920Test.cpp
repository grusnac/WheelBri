// The Linux virtual G920's uhid protocol (tools/uhid), without /dev/uhid: events in, events out.

#include "UhidG920.hpp"

#include "wheelbri/HidG920.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace wheelbri::uhid {
namespace {

std::vector<std::uint8_t> hex(const std::string& text) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

struct Kernel {
    std::vector<uhid_event> written;
    std::vector<FfbPacket> packets;
    std::vector<std::string> logs;
    UhidG920 device{[this](const uhid_event& e) { written.push_back(e); return true; },
                    [this](const FfbPacket& p) { packets.push_back(p); },
                    [this](LogLevel, const std::string& s) { logs.push_back(s); }};

    // What hidraw passes on: the report ID first.
    void output(const std::string& bytes, std::int64_t now) {
        uhid_event e{};
        e.type = UHID_OUTPUT;
        const auto b = hex(bytes);
        std::memcpy(e.u.output.data, b.data(), b.size());
        e.u.output.size = static_cast<std::uint16_t>(b.size());
        e.u.output.rtype = UHID_OUTPUT_REPORT;
        device.handle(e, now);
    }
    void setFeature(std::uint32_t id, const std::string& bytes, std::int64_t now) {
        uhid_event e{};
        e.type = UHID_SET_REPORT;
        const auto b = hex(bytes);
        e.u.set_report.id = id;
        e.u.set_report.rnum = b[0];
        e.u.set_report.rtype = UHID_FEATURE_REPORT;
        e.u.set_report.size = static_cast<std::uint16_t>(b.size());
        std::memcpy(e.u.set_report.data, b.data(), b.size());
        device.handle(e, now);
    }
    void getFeature(std::uint32_t id, std::uint8_t rnum) {
        uhid_event e{};
        e.type = UHID_GET_REPORT;
        e.u.get_report.id = id;
        e.u.get_report.rnum = rnum;
        e.u.get_report.rtype = UHID_FEATURE_REPORT;
        device.handle(e, 0);
    }
};

TEST(UhidG920, CreatesALogitechG920WithTheBridgeDescriptor) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    ASSERT_EQ(k.written.size(), 2U);
    const auto& create = k.written[0];
    EXPECT_EQ(create.type, UHID_CREATE2);
    EXPECT_EQ(create.u.create2.vendor, 0x046DU);
    EXPECT_EQ(create.u.create2.product, 0xC262U);
    EXPECT_EQ(create.u.create2.bus, 0x03);
    EXPECT_STREQ(reinterpret_cast<const char*>(create.u.create2.name), "Logitech G920 Driving Force Racing Wheel USB");
    const auto descriptor = g920ReportDescriptor();
    ASSERT_EQ(create.u.create2.rd_size, descriptor.size());
    EXPECT_EQ(std::memcmp(create.u.create2.rd_data, descriptor.data(), descriptor.size()), 0);

    const auto& input = k.written[1];   // centred wheel, released pedals at once
    EXPECT_EQ(input.type, UHID_INPUT2);
    const auto neutral = packG920Input(g920Neutral());
    ASSERT_EQ(input.u.input2.size, neutral.size());
    EXPECT_EQ(std::memcmp(input.u.input2.data, neutral.data(), neutral.size()), 0);
}

TEST(UhidG920, InputOnlyWhenChangedUnlessForced) {
    Kernel k;
    k.device.create();
    const std::size_t before = k.written.size();
    k.device.submit(g920Neutral(), false);
    EXPECT_EQ(k.written.size(), before);
    TargetReport turned = g920Neutral();
    turned.steering = 40000;
    k.device.submit(turned, false);
    EXPECT_EQ(k.written.size(), before + 1);
    k.device.submit(turned, true);
    EXPECT_EQ(k.written.size(), before + 2);
}

TEST(UhidG920, FeatureReportsAreAnswered) {
    Kernel k;
    k.device.create();
    k.written.clear();

    k.getFeature(7, HidPid::kPool);
    ASSERT_EQ(k.written.size(), 1U);
    EXPECT_EQ(k.written[0].type, UHID_GET_REPORT_REPLY);
    EXPECT_EQ(k.written[0].u.get_report_reply.id, 7U);
    EXPECT_EQ(k.written[0].u.get_report_reply.err, 0);
    EXPECT_EQ(k.written[0].u.get_report_reply.data[0], HidPid::kPool);

    k.setFeature(8, "110100", 0);       // Create New Effect: constant (report ID first, as hidraw sends it)
    ASSERT_EQ(k.written.size(), 2U);
    EXPECT_EQ(k.written[1].type, UHID_SET_REPORT_REPLY);
    EXPECT_EQ(k.written[1].u.set_report_reply.id, 8U);
    EXPECT_EQ(k.written[1].u.set_report_reply.err, 0);

    k.getFeature(9, HidPid::kBlockLoad);
    ASSERT_EQ(k.written.size(), 3U);
    EXPECT_EQ(k.written[2].u.get_report_reply.err, 0);
    EXPECT_EQ(k.written[2].u.get_report_reply.data[1], 1);   // EBI 1
    EXPECT_EQ(k.written[2].u.get_report_reply.data[2], 1);   // loaded

    k.getFeature(10, 0x42);             // not a report of this device
    EXPECT_NE(k.written[3].u.get_report_reply.err, 0);
}

// Wine's dinput (no device-managed pool in this descriptor): parameters and Set Effect with its own block
// numbers, then Effect Operation; the service gets a playing constant force.
TEST(UhidG920, OutputReportsBecomeForceFeedback) {
    Kernel k;
    k.device.create();
    k.output("1501A00F", 0);                                        // constant 4000, EBI 1
    k.output("110101DC05000000000000FFFF04FF1F0000FFFFFFFF", 0);    // Set Effect EBI 1
    k.output("1A010101", 1);                                        // start
    const FfbPacket* playing = nullptr;
    for (const auto& p : k.packets) {
        if (p.kind == FfbPacket::Kind::SlotState && p.playing) playing = &p;
    }
    ASSERT_NE(playing, nullptr);
    EXPECT_EQ(playing->effect.type, FfbEffectType::Constant);
    EXPECT_NEAR(playing->effect.constantLevel, 0.4F, 1e-3F);
    EXPECT_EQ(k.device.activeSlots(), 1U);
}

TEST(UhidG920, OpenAndCloseAreCounted) {
    Kernel k;
    uhid_event e{};
    e.type = UHID_START;
    k.device.handle(e, 0);
    e.type = UHID_OPEN;
    k.device.handle(e, 0);
    EXPECT_TRUE(k.device.started());
    EXPECT_EQ(k.device.openCount(), 1);
    e.type = UHID_CLOSE;
    k.device.handle(e, 0);
    EXPECT_EQ(k.device.openCount(), 0);
}

// The game exits or crashes: the kernel closes its handle, and its forces stop at once.
TEST(UhidG920, TheLastCloseStopsTheGamesForces) {
    Kernel k;
    k.device.create();
    uhid_event e{};
    e.type = UHID_OPEN;
    k.device.handle(e, 0);
    k.output("1501A00F", 0);
    k.output("110101DC05000000000000FFFF04FF1F0000FFFFFFFF", 0);
    k.output("1A010101", 1);
    ASSERT_EQ(k.device.activeSlots(), 1U);
    k.packets.clear();
    e.type = UHID_CLOSE;
    k.device.handle(e, 100);
    EXPECT_EQ(k.device.activeSlots(), 0U);
    bool stopAll = false;
    for (const auto& p : k.packets) stopAll = stopAll || p.kind == FfbPacket::Kind::StopAll;
    EXPECT_TRUE(stopAll);
}

TEST(UhidG920, AnUnsupportedReportIsLoggedOnce) {
    Kernel k;
    k.device.create();
    k.logs.clear();
    k.getFeature(1, 0x42);
    k.getFeature(2, 0x42);
    k.getFeature(3, 0x42);
    EXPECT_EQ(k.written.back().u.get_report_reply.id, 3U);   // every request is still answered
    EXPECT_EQ(std::count_if(k.logs.begin(), k.logs.end(),
                            [](const std::string& s) { return s.find("not supported") != std::string::npos; }),
              1);
}

}  // namespace
}  // namespace wheelbri::uhid
