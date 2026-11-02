// The Linux virtual wheel's uhid protocol (tools/uhid), without /dev/uhid: events in, events out.

#include "UhidWheel.hpp"

#include "wheelbri/HidWheelDevice.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
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
    UhidWheel device;

    explicit Kernel(const VirtualWheel& wheel = *virtualWheel("g920"))
        : device{[this](const uhid_event& e) { written.push_back(e); return true; },
                 [this](const FfbPacket& p) { packets.push_back(p); },
                 [this](LogLevel, const std::string& s) { logs.push_back(s); }, wheel} {}

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

// Its descriptor is the Windows one, the combined pedals' Slider 0 and the clutch's Slider 1 included: Proton's
// dinput numbers Sliders as Windows does, and ACC's G25 preset reads the clutch on Slider 1 (2026-10-26).
TEST(UhidWheel, CreatesALogitechG25OnTheVirtualBusWithTheWindowsDescriptor) {
    Kernel k(*virtualWheel("g25"));
    ASSERT_TRUE(k.device.create());
    ASSERT_EQ(k.written.size(), 2U);
    const auto& create = k.written[0].u.create2;
    EXPECT_EQ(create.vendor, 0x046DU);
    EXPECT_EQ(create.product, 0xC299U);
    EXPECT_EQ(create.bus, 0x06);   // BUS_VIRTUAL: hid-lg would claim (and then refuse) a USB 046D:C299
    EXPECT_STREQ(reinterpret_cast<const char*>(create.name), "Logitech G25 Racing Wheel USB");
    EXPECT_STREQ(reinterpret_cast<const char*>(create.uniq), kVirtualWheelUniq);   // the service never reads it
    // Proton has no G25 hack: X, Y, Rz, Slider (combined pedals), Slider (clutch) as on Windows.
    const auto descriptor = reportDescriptorFor(*virtualWheel("g25"));
    ASSERT_EQ(descriptor.size(), wheelReportDescriptor().size() + 16);   // the fifth axis
    ASSERT_EQ(create.rd_size, descriptor.size());
    EXPECT_EQ(std::memcmp(create.rd_data, descriptor.data(), descriptor.size()), 0);
    EXPECT_EQ(k.device.label(), "G25");
    EXPECT_EQ(k.device.name(), "Logitech G25 Racing Wheel USB");

    // The input report matches it: 15 bytes, the clutch after the combined pedals.
    k.written.clear();
    TargetReport report = neutralReport();
    report.clutch = 1000;
    ASSERT_TRUE(k.device.submit(report, false));
    ASSERT_EQ(k.written.size(), 1U);
    const auto& input = k.written[0].u.input2;
    ASSERT_EQ(input.size, kMaxInputReportSize);
    const auto plain = packWheelInput(report);
    EXPECT_EQ(input.data[9], plain[7]);   // the clutch: Slider 1
    EXPECT_EQ(input.data[10], plain[8]);
}

TEST(UhidWheel, CreatesALogitechG920WithTheProtonDescriptor) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    ASSERT_EQ(k.written.size(), 2U);
    const auto& create = k.written[0];
    EXPECT_EQ(create.type, UHID_CREATE2);
    EXPECT_EQ(create.u.create2.vendor, 0x046DU);
    EXPECT_EQ(create.u.create2.product, 0xC262U);
    EXPECT_EQ(create.u.create2.bus, 0x03);
    EXPECT_STREQ(reinterpret_cast<const char*>(create.u.create2.name), "Logitech G920 Driving Force Racing Wheel USB");
    const auto descriptor = g920ProtonReportDescriptor();
    ASSERT_EQ(create.u.create2.rd_size, descriptor.size());
    EXPECT_EQ(std::memcmp(create.u.create2.rd_data, descriptor.data(), descriptor.size()), 0);

    const auto& input = k.written[1];   // centred wheel, released pedals at once
    EXPECT_EQ(input.type, UHID_INPUT2);
    const auto neutral = packWheelInput(neutralReport());
    ASSERT_EQ(input.u.input2.size, neutral.size());
    EXPECT_EQ(std::memcmp(input.u.input2.data, neutral.data(), neutral.size()), 0);
}

TEST(UhidWheel, InputOnlyWhenChangedUnlessForced) {
    Kernel k;
    k.device.create();
    const std::size_t before = k.written.size();
    k.device.submit(neutralReport(), false);
    EXPECT_EQ(k.written.size(), before);
    TargetReport turned = neutralReport();
    turned.steering = 40000;
    k.device.submit(turned, false);
    EXPECT_EQ(k.written.size(), before + 1);
    k.device.submit(turned, true);
    EXPECT_EQ(k.written.size(), before + 2);
}

TEST(UhidWheel, FeatureReportsAreAnswered) {
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
TEST(UhidWheel, OutputReportsBecomeForceFeedback) {
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

// The G923 has HID++ for G HUB (report 0x11): a request comes back as an input report (0x12). Its HID PID reports are 0x10
// higher, so the same force feedback as the G920's arrives with 0x25, 0x21, 0x2A; feature reports answer 0x2x.
TEST(UhidWheel, TheG923AnswersHidppAndPlaysPidFromItsMovedReports) {
    Kernel k(*virtualWheel("g923"));
    ASSERT_TRUE(k.device.create());
    EXPECT_EQ(k.written[0].u.create2.rd_size, reportDescriptorFor(*virtualWheel("g923")).size());
    k.written.clear();
    k.output("11FF001100005A" + std::string(26, '0'), 0);   // Root.getProtocolVersion, ping 0x5A
    ASSERT_EQ(hex("11FF001100005A" + std::string(26, '0')).size(), HidPp::kLongSize);
    ASSERT_EQ(k.written.size(), 1U);
    ASSERT_EQ(k.written[0].type, UHID_INPUT2);
    EXPECT_EQ(k.written[0].u.input2.size, HidPp::kVeryLongSize);   // answered very long, as a real G923
    EXPECT_EQ(k.written[0].u.input2.data[0], 0x12);
    EXPECT_EQ(k.written[0].u.input2.data[4], 4);   // protocol 4.2
    EXPECT_EQ(k.written[0].u.input2.data[5], 2);
    EXPECT_EQ(k.written[0].u.input2.data[6], 0x5A);

    k.output("2501A00F", 0);                                        // constant 4000, EBI 1
    k.output("210101DC05000000000000FFFF04FF1F0000FFFFFFFF", 0);    // Set Effect EBI 1
    k.output("2A010101", 1);                                        // start
    bool playing = false;
    for (const auto& p : k.packets) playing = playing || (p.kind == FfbPacket::Kind::SlotState && p.playing);
    EXPECT_TRUE(playing);

    k.written.clear();
    k.getFeature(3, static_cast<std::uint8_t>(HidPid::kPool + kHidPpPidShift));
    ASSERT_EQ(k.written.size(), 1U);
    EXPECT_EQ(k.written[0].u.get_report_reply.err, 0);
    EXPECT_EQ(k.written[0].u.get_report_reply.data[0], HidPid::kPool + kHidPpPidShift);
}

// The G25 plays the game's force feedback the same way, and its log names it.
TEST(UhidWheel, TheG25PlaysForceFeedbackAndSaysWhoItIs) {
    Kernel k(*virtualWheel("g25"));
    ASSERT_TRUE(k.device.create());
    uhid_event e{};
    e.type = UHID_START;
    k.device.handle(e, 0);
    e.type = UHID_OPEN;
    k.device.handle(e, 0);
    k.output("1501A00F", 0);
    k.output("110101DC05000000000000FFFF04FF1F0000FFFFFFFF", 0);
    k.output("1A010101", 1);
    EXPECT_EQ(k.device.activeSlots(), 1U);
    e.type = UHID_CLOSE;
    k.device.handle(e, 2);
    e.type = UHID_STOP;
    k.device.handle(e, 3);
    const auto said = [&k](const std::string& text) {
        return std::any_of(k.logs.begin(), k.logs.end(), [&](const std::string& l) { return l == text; });
    };
    EXPECT_TRUE(said("uhid: virtual G25 started (the kernel took it)"));
    EXPECT_TRUE(said("uhid: a program opened the virtual G25 (1 now)"));
    EXPECT_TRUE(said("uhid: a program closed the virtual G25 (0 left)"));
    EXPECT_TRUE(said("uhid: virtual G25 stopped by the kernel"));
    EXPECT_EQ(k.device.activeSlots(), 0U);   // the last close stopped its forces
    // Its input reports are the Windows ones (the same descriptor, with the combined pedals and the clutch).
    TargetReport report = neutralReport();
    report.buttons = 1U << kG25FirstGear;
    k.written.clear();
    ASSERT_TRUE(k.device.submit(report, false));
    ASSERT_EQ(k.written.size(), 1U);
    const InputReport packed = packInputFor(*virtualWheel("g25"), report);
    ASSERT_EQ(k.written[0].u.input2.size, packed.size);
    EXPECT_EQ(std::memcmp(k.written[0].u.input2.data, packed.bytes.data(), packed.size), 0);
}

TEST(UhidWheel, OpenAndCloseAreCounted) {
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
    k.device.handle(e, 0);   // a close without an open: never below 0
    EXPECT_EQ(k.device.openCount(), 0);
}

// The game exits or crashes: the kernel closes its handle, and its forces stop at once.
TEST(UhidWheel, TheLastCloseStopsTheGamesForces) {
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

// The length Linux's hid-logitech-hidpp gives output report `id` (hidpp_get_report_length): the first field's
// Report Count + 1, a field being an Output item with usages (padding has none and makes no field); 0 if absent.
std::size_t hidppReportLength(const std::span<const std::uint8_t> descriptor, const std::uint8_t id) {
    struct Globals {
        std::uint32_t count = 0;
        std::uint8_t reportId = 0;
    };
    Globals globals;
    std::vector<Globals> stack;
    std::size_t usages = 0;
    std::uint32_t usageMinimum = 0;
    for (std::size_t i = 0; i < descriptor.size();) {
        const std::uint8_t prefix = descriptor[i];
        if (prefix == 0xFE) {   // long item: its data size follows
            i += 3 + (i + 1 < descriptor.size() ? descriptor[i + 1] : 0);
            continue;
        }
        const std::size_t size = (prefix & 3U) == 3 ? 4 : (prefix & 3U);
        std::uint32_t value = 0;
        for (std::size_t b = 0; b < size && i + 1 + b < descriptor.size(); ++b) {
            value |= static_cast<std::uint32_t>(descriptor[i + 1 + b]) << (8 * b);
        }
        switch (prefix & 0xFCU) {
            case 0x94: globals.count = value; break;                                     // Report Count
            case 0x84: globals.reportId = static_cast<std::uint8_t>(value); break;       // Report ID
            case 0xA4: stack.push_back(globals); break;                                  // Push
            case 0xB4: if (!stack.empty()) { globals = stack.back(); stack.pop_back(); } break;   // Pop
            case 0x08: ++usages; break;                                                  // Usage
            case 0x18: usageMinimum = value; break;                                      // Usage Minimum
            case 0x28: usages += value >= usageMinimum ? value - usageMinimum + 1 : 0; break;   // Usage Maximum
            case 0x90:                                                                   // Output
                if (usages > 0 && globals.reportId == id) return globals.count + 1;
                [[fallthrough]];
            case 0x80: case 0xB0: case 0xA0: case 0xC0:                                  // the other main items
                usages = 0;
                usageMinimum = 0;
                break;
            default: break;
        }
        i += 1 + size;
    }
    return 0;
}

// The kernel's hid-logitech-hidpp claims 046D:C262 and, when output reports 0x10 / 0x11 / 0x12 look like HID++ ones,
// sends HID++ commands to the device, which HidPid would take for the game's force feedback. It gives up (and leaves
// the device to the generic HID driver: "not enough values in hidpp report 17") when one of them is there but shorter
// than HID++ needs (hidpp_validate_device): keep it so whenever the descriptor changes.
TEST(UhidWheel, TheKernelsHidppDriverLeavesTheDeviceAlone) {
    const auto descriptor = g920ProtonReportDescriptor();
    constexpr std::array<std::pair<std::uint8_t, std::size_t>, 3> kHidppReports{{
        {0x10, 7},    // HIDPP_REPORT_SHORT_LENGTH
        {0x11, 20},   // HIDPP_REPORT_LONG_LENGTH
        {0x12, 20},   // very long: at least HIDPP_REPORT_LONG_LENGTH
    }};
    bool refused = false;
    for (const auto& [id, needed] : kHidppReports) {
        const std::size_t length = hidppReportLength(descriptor, id);
        refused = refused || (length > 0 && length < needed);
    }
    EXPECT_TRUE(refused) << "hid-logitech-hidpp would take the virtual G920 for a HID++ device";
    EXPECT_GT(hidppReportLength(descriptor, 0x11), 0U);   // the report that makes it give up today
    EXPECT_LT(hidppReportLength(descriptor, 0x11), 20U);
}

// The service's evdev listing leaves a device with this uniq out (WheelEvdev.cpp): it must be what uhid sets.
TEST(UhidWheel, MarksItselfSoTheServiceNeverReadsItAsARig) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    EXPECT_STREQ(reinterpret_cast<const char*>(k.written[0].u.create2.uniq), kVirtualWheelUniq);
    EXPECT_STREQ(reinterpret_cast<const char*>(k.written[0].u.create2.phys), "wheelbri");
}

TEST(UhidWheel, ARefusedCreateIsReported) {
    UhidWheel device{[](const uhid_event&) { return false; }, [](const FfbPacket&) {}, [](LogLevel, const std::string&) {}};
    EXPECT_FALSE(device.create());
}

TEST(UhidWheel, StopAndDestroy) {
    Kernel k;
    k.device.create();
    uhid_event e{};
    e.type = UHID_START;
    k.device.handle(e, 0);
    ASSERT_TRUE(k.device.started());
    e.type = UHID_STOP;
    k.device.handle(e, 0);
    EXPECT_FALSE(k.device.started());
    k.device.destroy();
    EXPECT_EQ(k.written.back().type, UHID_DESTROY);
}

// Some programs read the input report through a get-report request instead of waiting for it.
TEST(UhidWheel, TheInputReportCanBeAskedFor) {
    Kernel k;
    const auto getInput = [&k](std::uint32_t id) {
        uhid_event e{};
        e.type = UHID_GET_REPORT;
        e.u.get_report.id = id;
        e.u.get_report.rnum = kWheelInputReportId;
        e.u.get_report.rtype = UHID_INPUT_REPORT;
        k.device.handle(e, 0);
        return k.written.back().u.get_report_reply;
    };
    auto reply = getInput(7);   // nothing submitted yet: an error, still answered
    EXPECT_EQ(reply.id, 7U);
    EXPECT_EQ(reply.err, EIO);
    EXPECT_EQ(reply.size, 0U);

    k.device.create();
    TargetReport turned = neutralReport();
    turned.steering = 50000;
    k.device.submit(turned, false);
    reply = getInput(8);
    EXPECT_EQ(reply.err, 0);
    const auto expected = packWheelInput(turned);
    ASSERT_EQ(reply.size, expected.size());
    EXPECT_EQ(std::memcmp(reply.data, expected.data(), expected.size()), 0);
}

// Output reports may also come as SET_REPORT (hidraw's HIDIOCSOUTPUT): the same force feedback, and an answer.
TEST(UhidWheel, OutputReportsBySetReportAlsoBecomeForceFeedback) {
    Kernel k;
    k.device.create();
    const auto setOutput = [&k](std::uint32_t id, const std::string& bytes) {
        uhid_event e{};
        e.type = UHID_SET_REPORT;
        const auto b = hex(bytes);
        e.u.set_report.id = id;
        e.u.set_report.rnum = b[0];
        e.u.set_report.rtype = UHID_OUTPUT_REPORT;
        e.u.set_report.size = static_cast<std::uint16_t>(b.size());
        std::memcpy(e.u.set_report.data, b.data(), b.size());
        k.device.handle(e, 0);
        return k.written.back();
    };
    setOutput(1, "1501A00F");
    setOutput(2, "110101DC05000000000000FFFF04FF1F0000FFFFFFFF");
    const auto reply = setOutput(3, "1A010101");
    EXPECT_EQ(reply.type, UHID_SET_REPORT_REPLY);
    EXPECT_EQ(reply.u.set_report_reply.id, 3U);
    EXPECT_EQ(reply.u.set_report_reply.err, 0);
    EXPECT_EQ(k.device.activeSlots(), 1U);
}

TEST(UhidWheel, AnEmptyOrOversizedOutputReportIsHarmless) {
    Kernel k;
    k.device.create();
    const std::size_t before = k.written.size();
    uhid_event e{};
    e.type = UHID_OUTPUT;
    e.u.output.size = 0;
    k.device.handle(e, 0);
    e.u.output.size = 0xFFFF;   // more than the event holds: cut to its buffer, never read past it
    e.u.output.data[0] = 0x7E;  // an id HidPid does not know
    k.device.handle(e, 0);
    EXPECT_EQ(k.written.size(), before);   // nothing to answer
    EXPECT_EQ(k.packets.size(), 0U);
    e.type = 0xFFFF;                       // an event type this kernel does not have yet
    k.device.handle(e, 0);
    EXPECT_EQ(k.written.size(), before);
}

TEST(UhidWheel, AnUnsupportedReportIsLoggedOnce) {
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

uhid_event setReport(const std::uint32_t id, const std::uint8_t rnum, const std::uint8_t rtype,
                     const std::vector<std::uint8_t>& data) {
    uhid_event e{};
    e.type = UHID_SET_REPORT;
    e.u.set_report.id = id;
    e.u.set_report.rnum = rnum;
    e.u.set_report.rtype = rtype;
    e.u.set_report.size = static_cast<std::uint16_t>(data.size());
    if (!data.empty()) std::memcpy(e.u.set_report.data, data.data(), data.size());   // data() may be null when empty
    return e;
}

// The G923's HID++ requests can also come as SET_REPORT (hidraw's HIDIOCSOUTPUT), with or without the report ID
// first; a request too short for HID++ is not answered; a G920 PID id below the moved range goes nowhere.
TEST(UhidWheel, TheG923TakesHidppBySetReportAndIgnoresStrayReports) {
    Kernel k(*virtualWheel("g923"));
    ASSERT_TRUE(k.device.create());
    k.written.clear();
    const auto request = hex("11FF001100005A" + std::string(26, '0'));
    k.device.handle(setReport(1, 0x11, UHID_OUTPUT_REPORT, request), 0);   // with the ID
    ASSERT_EQ(k.written.size(), 2U);
    EXPECT_EQ(k.written[0].type, UHID_INPUT2);
    EXPECT_EQ(k.written[0].u.input2.data[6], 0x5A);
    EXPECT_EQ(k.written[1].type, UHID_SET_REPORT_REPLY);
    k.written.clear();
    const std::vector<std::uint8_t> noId(request.begin() + 1, request.end());
    k.device.handle(setReport(2, 0x11, UHID_OUTPUT_REPORT, noId), 0);      // without it: the same answer
    ASSERT_EQ(k.written.size(), 2U);
    EXPECT_EQ(k.written[0].u.input2.data[0], 0x12);
    EXPECT_EQ(k.written[0].u.input2.data[6], 0x5A);

    k.written.clear();
    k.output("11FF", 0);         // too short for HID++: no answer
    k.output("1501A00F", 0);     // the G920's PID id: not one the G923 has
    EXPECT_TRUE(k.written.empty());
    EXPECT_TRUE(k.packets.empty());
    k.device.handle(setReport(3, 0x15, UHID_OUTPUT_REPORT, hex("1501A00F")), 0);
    k.device.handle(setReport(4, 0x11, UHID_FEATURE_REPORT, request), 0);   // HID++ as a feature: not HID++
    k.device.handle(setReport(5, 0x15, UHID_FEATURE_REPORT, hex("1501")), 0);
    EXPECT_TRUE(k.packets.empty());
    ASSERT_EQ(k.written.size(), 3U);   // only the three replies
    for (const uhid_event& e : k.written) EXPECT_EQ(e.type, UHID_SET_REPORT_REPLY);

    k.written.clear();
    k.getFeature(6, 0x11);       // a HID++ id as a feature: not supported, still answered
    ASSERT_EQ(k.written.size(), 1U);
    EXPECT_EQ(k.written[0].u.get_report_reply.err, EIO);
}

// A SET_REPORT of a kind the device has no use for (an input report) is answered and changes nothing.
TEST(UhidWheel, ASetInputReportIsAnsweredAndIgnored) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    k.written.clear();
    k.device.handle(setReport(9, 0x1A, UHID_INPUT_REPORT, hex("1A010101")), 0);
    ASSERT_EQ(k.written.size(), 1U);
    EXPECT_EQ(k.written[0].type, UHID_SET_REPORT_REPLY);
    EXPECT_EQ(k.written[0].u.set_report_reply.id, 9U);
    EXPECT_TRUE(k.packets.empty());
}

// Get-report requests for reports the device does not serve: an output report, an input report under another ID,
// a report type the kernel may add later. Each is answered with an error and named once in the log.
TEST(UhidWheel, UnservedGetReportsAreAnsweredWithAnError) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    k.logs.clear();
    k.written.clear();
    const auto get = [&k](const std::uint32_t id, const std::uint8_t rnum, const std::uint8_t rtype) {
        uhid_event e{};
        e.type = UHID_GET_REPORT;
        e.u.get_report.id = id;
        e.u.get_report.rnum = rnum;
        e.u.get_report.rtype = rtype;
        k.device.handle(e, 0);
        return k.written.back().u.get_report_reply;
    };
    EXPECT_EQ(get(1, 0x1A, UHID_OUTPUT_REPORT).err, EIO);
    EXPECT_EQ(get(2, kWheelInputReportId + 1, UHID_INPUT_REPORT).err, EIO);
    EXPECT_EQ(get(3, 0x01, 0x7F).err, EIO);
    const auto said = [&k](const std::string& text) {
        return std::any_of(k.logs.begin(), k.logs.end(), [&](const std::string& l) { return l.find(text) == 0; });
    };
    EXPECT_TRUE(said("uhid: get output report 26"));
    EXPECT_TRUE(said("uhid: get input report " + std::to_string(kWheelInputReportId + 1)));
    EXPECT_TRUE(said("uhid: get ? report 1"));
}

// Two programs have the wheel open (the game and a tester): one closing leaves the game's forces playing.
TEST(UhidWheel, ACloseWhileAnotherProgramReadsKeepsTheForces) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    uhid_event e{};
    e.type = UHID_OPEN;
    k.device.handle(e, 0);
    k.device.handle(e, 0);
    k.output("1501A00F", 0);
    k.output("110101DC05000000000000FFFF04FF1F0000FFFFFFFF", 0);
    k.output("1A010101", 1);
    ASSERT_EQ(k.device.activeSlots(), 1U);
    e.type = UHID_CLOSE;
    k.device.handle(e, 2);
    EXPECT_EQ(k.device.openCount(), 1);
    EXPECT_EQ(k.device.activeSlots(), 1U);
    k.device.handle(e, 3);
    EXPECT_EQ(k.device.activeSlots(), 0U);
    k.device.handle(e, 4);   // a close with nothing open and nothing playing
    EXPECT_EQ(k.device.openCount(), 0);
}

// A report submitted before create() (the loop's first state arrives early) is sent, not taken for a repeat.
TEST(UhidWheel, TheFirstSubmitIsAlwaysSent) {
    Kernel k;
    EXPECT_TRUE(k.device.submit(neutralReport(), false));
    EXPECT_FALSE(k.device.submit(neutralReport(), false));
    EXPECT_TRUE(k.device.submit(neutralReport(), true));
    EXPECT_EQ(k.written.size(), 2U);
}

// An empty SET_REPORT (a program probing the device): answered, nothing decoded.
TEST(UhidWheel, AnEmptySetReportIsAnswered) {
    Kernel k;
    ASSERT_TRUE(k.device.create());
    k.written.clear();
    k.device.handle(setReport(4, 0x15, UHID_FEATURE_REPORT, {}), 0);
    k.device.handle(setReport(5, 0x15, UHID_OUTPUT_REPORT, {}), 0);
    ASSERT_EQ(k.written.size(), 2U);
    EXPECT_EQ(k.written[1].u.set_report_reply.id, 5U);
    EXPECT_EQ(k.written[1].u.set_report_reply.err, 0);
    EXPECT_TRUE(k.packets.empty());
}

}  // namespace
}  // namespace wheelbri::uhid
