#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidPp.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

struct Wheel {
    std::vector<std::string> lines;
    std::vector<std::pair<int, float>> settings;
    HidPp hidpp{HidPp::Identity{"G923 Racing Wheel for Xbox One and PC", 0xC26E, 0x57420001, 0x0A},
                [this](LogLevel level, const std::string& line) {
                    if (level >= LogLevel::Info) lines.push_back(line);
                },
                [this](const int degrees, const float gain) { settings.emplace_back(degrees, gain); }};

    // A long request: [0x11, 0xFF, feature index, function << 4 | sw id 1, params...].
    std::vector<std::uint8_t> ask(const std::uint8_t index, const std::uint8_t function,
                                  const std::vector<std::uint8_t>& params = {}, const std::uint8_t id = 0x11) {
        std::vector<std::uint8_t> request(id == 0x11 ? HidPp::kLongSize : HidPp::kVeryLongSize, 0);
        request[0] = id;
        request[1] = 0xFF;
        request[2] = index;
        request[3] = static_cast<std::uint8_t>(function << 4U | 1U);
        for (std::size_t i = 0; i < params.size(); ++i) request[4 + i] = params[i];
        const auto answer = hidpp.handle(request, 0);
        return answer ? *answer : std::vector<std::uint8_t>{};
    }
    // Root.getFeature(id) -> its index (0: not there).
    std::uint8_t indexOf(const std::uint16_t id) {
        return ask(0, 0, {static_cast<std::uint8_t>(id >> 8U), static_cast<std::uint8_t>(id & 0xFFU)})[4];
    }
};

TEST(HidPp, PingAnswersProtocol42AndEchoesTheHeader) {
    Wheel w;
    const auto answer = w.ask(0, 1, {0, 0, 0x5A});
    ASSERT_EQ(answer.size(), HidPp::kLongSize);
    EXPECT_EQ(answer[0], 0x11);
    EXPECT_EQ(answer[1], 0xFF);   // the device index, as asked
    EXPECT_EQ(answer[2], 0x00);
    EXPECT_EQ(answer[3], 0x11);   // function 1, software id 1
    EXPECT_EQ(answer[4], 4);
    EXPECT_EQ(answer[5], 2);
    EXPECT_EQ(answer[6], 0x5A);   // the ping byte back
}

TEST(HidPp, RootFindsItsFeaturesAndSaysWhatIsNot) {
    Wheel w;
    EXPECT_EQ(w.indexOf(HidPp::kFeatureSet), 1);
    EXPECT_EQ(w.indexOf(HidPp::kFwVersion), 2);
    EXPECT_EQ(w.indexOf(HidPp::kNameType), 3);
    EXPECT_EQ(w.indexOf(HidPp::kForceFeedback), 4);
    EXPECT_EQ(w.indexOf(0x807A), 0);   // rev lights: not there
    // What was asked for and is not there reaches the log, once.
    EXPECT_EQ(w.indexOf(0x807A), 0);
    int mentions = 0;
    for (const std::string& line : w.lines) mentions += line.find("0x807A") != std::string::npos ? 1 : 0;
    EXPECT_EQ(mentions, 1);
}

TEST(HidPp, FeatureSetListsEveryFeature) {
    Wheel w;
    EXPECT_EQ(w.ask(1, 0)[4], 4);   // 4 besides Root
    const std::map<std::uint8_t, std::uint16_t> expected = {
        {0, HidPp::kRoot}, {1, HidPp::kFeatureSet}, {2, HidPp::kFwVersion}, {3, HidPp::kNameType},
        {4, HidPp::kForceFeedback}};
    for (const auto& [index, id] : expected) {
        const auto answer = w.ask(1, 1, {index});
        EXPECT_EQ((answer[4] << 8U) | answer[5], id) << int(index);
    }
    const auto past = w.ask(1, 1, {5});
    EXPECT_EQ(past[2], 0xFF);   // an error
    EXPECT_EQ(past[5], HidPp::kErrOutOfRange);
}

TEST(HidPp, TheNameComesInPieces) {
    Wheel w;
    const std::string expected = "G923 Racing Wheel for Xbox One and PC";
    const std::size_t length = w.ask(3, 0)[4];
    ASSERT_EQ(length, expected.size());
    std::string name;
    while (name.size() < length) {
        const auto answer = w.ask(3, 1, {static_cast<std::uint8_t>(name.size())});
        for (std::size_t i = 4; i < answer.size() && name.size() < length; ++i) name += static_cast<char>(answer[i]);
    }
    EXPECT_EQ(name, expected);
    EXPECT_EQ(w.ask(3, 2)[4], 0x0A);   // a steering wheel
}

TEST(HidPp, FirmwareSaysUsbItsModelAndAVersionNoUpdateBeats) {
    Wheel w;
    const auto entities = w.ask(2, 0);
    EXPECT_EQ(entities[4], 1);
    EXPECT_EQ(entities[5], 0x57);   // unit id
    EXPECT_EQ(entities[8], 0x01);
    EXPECT_EQ((entities[9] << 8U) | entities[10], 0x0008);    // USB
    EXPECT_EQ((entities[11] << 8U) | entities[12], 0xC26E);   // model: the product id
    const auto fw = w.ask(2, 1, {0});
    EXPECT_EQ(std::string(fw.begin() + 5, fw.begin() + 8), "WBR");
    EXPECT_EQ(fw[8], 0x99);
    EXPECT_EQ(w.ask(2, 1, {1})[5], HidPp::kErrOutOfRange);   // one entity only
}

TEST(HidPp, RotationAndGainAreKeptReadBackAndReported) {
    Wheel w;
    auto answer = w.ask(4, 5);
    EXPECT_EQ((answer[4] << 8U) | answer[5], 900);
    w.ask(4, 6, {0x01, 0x0E});   // 270
    answer = w.ask(4, 5);
    EXPECT_EQ((answer[4] << 8U) | answer[5], 270);
    EXPECT_EQ(w.hidpp.rotationDeg(), 270);
    EXPECT_EQ(w.ask(4, 6, {0x00, 0x05})[5], HidPp::kErrInvalidArgument);   // 5 deg: refused
    EXPECT_EQ(w.hidpp.rotationDeg(), 270);
    w.ask(4, 8, {0x80, 0x00, 0x00, 0x00});
    EXPECT_NEAR(w.hidpp.gain(), 0.5F, 0.001F);
    answer = w.ask(4, 7);
    EXPECT_EQ((answer[4] << 8U) | answer[5], 0x8000);
    ASSERT_EQ(w.settings.size(), 2U);
    EXPECT_EQ(w.settings[0].first, 270);
    EXPECT_NEAR(w.settings[1].second, 0.5F, 0.001F);
}

TEST(HidPp, EffectsGetSlotsButAreNotPlayed) {
    Wheel w;
    EXPECT_EQ(w.ask(4, 0)[4], HidPp::kEffectSlots);
    EXPECT_EQ(w.ask(4, 2, {0x01, 0x02})[4], 1);
    EXPECT_EQ(w.ask(4, 2, {0x01, 0x02})[4], 2);
    EXPECT_EQ(w.ask(4, 3, {1, 1})[2], 4);   // acknowledged, no error
    w.ask(4, 1);                             // reset: slots from 1 again
    EXPECT_EQ(w.ask(4, 2, {0x01})[4], 1);
}

TEST(HidPp, ErrorsForUnknownIndexesAndFunctions) {
    Wheel w;
    auto answer = w.ask(9, 0);
    EXPECT_EQ(answer[2], 0xFF);
    EXPECT_EQ(answer[3], 9);      // the feature index asked
    EXPECT_EQ(answer[4], 0x01);   // function 0, sw id 1
    EXPECT_EQ(answer[5], HidPp::kErrInvalidFeatureIndex);
    answer = w.ask(0, 7);
    EXPECT_EQ(answer[5], HidPp::kErrInvalidFunction);
}

TEST(HidPp, Hidpp10RegisterRequestsGetTheOldError) {
    Wheel w;
    const auto answer = w.ask(0x81, 0x0F);
    EXPECT_EQ(answer[2], 0x8F);
    EXPECT_EQ(answer[3], 0x81);
    EXPECT_EQ(answer[5], 0x01);   // invalid sub id
}

TEST(HidPp, VeryLongRequestsGetVeryLongAnswersAndOthersNone) {
    Wheel w;
    const auto answer = w.ask(0, 1, {0, 0, 7}, 0x12);
    ASSERT_EQ(answer.size(), HidPp::kVeryLongSize);
    EXPECT_EQ(answer[0], 0x12);
    EXPECT_EQ(answer[6], 7);
    const std::vector<std::uint8_t> pid = {0x21, 1, 2, 3, 4};
    EXPECT_FALSE(w.hidpp.handle(pid, 0).has_value());
    const std::vector<std::uint8_t> shortOne = {0x11, 0xFF};
    EXPECT_FALSE(w.hidpp.handle(shortOne, 0).has_value());
}


// A request of only its header (a host that sends no parameters): every function reads the missing ones as 0.
TEST(HidPp, RequestsWithoutParametersReadZeros) {
    Wheel w;
    const auto bare = [&w](const std::uint8_t index, const std::uint8_t function) {
        const std::vector<std::uint8_t> request = {0x11, 0xFF, index, static_cast<std::uint8_t>(function << 4U | 1U)};
        const auto answer = w.hidpp.handle(request, 0);
        return answer ? *answer : std::vector<std::uint8_t>{};
    };
    auto answer = bare(0, 0);   // getFeature(0x0000): Root, at index 0
    ASSERT_EQ(answer.size(), HidPp::kLongSize);
    EXPECT_EQ(answer[2], 0);
    EXPECT_EQ(answer[4], 0);
    EXPECT_EQ(bare(0, 1)[6], 0);                          // ping without its byte
    EXPECT_EQ((bare(1, 1)[4] << 8U) | bare(1, 1)[5], 0);  // getFeatureId(0): Root
    EXPECT_EQ(bare(2, 1)[5], 'W');                        // getFwInfo(0)
    EXPECT_EQ(bare(3, 1)[4], 'G');                        // getDeviceName(0)
    EXPECT_EQ(bare(4, 2)[4], 1);                          // an effect with no bytes still gets a slot
    EXPECT_EQ(bare(4, 3)[2], 4);                          // its state: acknowledged
    EXPECT_EQ(bare(4, 6)[5], HidPp::kErrInvalidArgument); // aperture 0: refused
    EXPECT_EQ(w.hidpp.rotationDeg(), 900);
    bare(4, 8);                                           // gains 0
    EXPECT_EQ(w.hidpp.gain(), 0.0F);
    EXPECT_EQ(w.hidpp.requests(), 10U);
    bool asked = false;
    for (const std::string& line : w.lines) asked = asked || line.find("Root") != std::string::npos;
    EXPECT_TRUE(asked);
}

// Functions a feature does not have get "invalid function", each feature on its own.
TEST(HidPp, EveryFeatureRefusesFunctionsItDoesNotHave) {
    Wheel w;
    EXPECT_EQ(w.ask(1, 2)[5], HidPp::kErrInvalidFunction);
    EXPECT_EQ(w.ask(2, 2)[5], HidPp::kErrInvalidFunction);
    EXPECT_EQ(w.ask(3, 3)[5], HidPp::kErrInvalidFunction);
    const auto answer = w.ask(4, 9);
    EXPECT_EQ(answer[2], 0xFF);
    EXPECT_EQ(answer[3], 4);
    EXPECT_EQ(answer[5], HidPp::kErrInvalidFunction);
    w.ask(4, 9);
    int mentions = 0;   // said in the log once
    for (const std::string& line : w.lines) mentions += line.find("function 9") != std::string::npos ? 1 : 0;
    EXPECT_EQ(mentions, 1);
}

// The name asked from past its end is out of range; from its very end it is empty (nothing more to read).
TEST(HidPp, TheNameFromPastItsEnd) {
    Wheel w;
    const auto size = static_cast<std::uint8_t>(std::string("G923 Racing Wheel for Xbox One and PC").size());
    const auto atEnd = w.ask(3, 1, {size});
    EXPECT_EQ(atEnd[2], 3);
    EXPECT_EQ(atEnd[4], 0);
    EXPECT_EQ(w.ask(3, 1, {static_cast<std::uint8_t>(size + 1)})[5], HidPp::kErrOutOfRange);
}

// G HUB's rotation range is 40..2520 degrees: both ends are taken, one past either is refused.
TEST(HidPp, RotationLimitsAreInclusive) {
    Wheel w;
    EXPECT_EQ(w.ask(4, 6, {0x00, 40})[2], 4);
    EXPECT_EQ(w.hidpp.rotationDeg(), 40);
    EXPECT_EQ(w.ask(4, 6, {0x09, 0xD8})[2], 4);   // 2520
    EXPECT_EQ(w.hidpp.rotationDeg(), 2520);
    EXPECT_EQ(w.ask(4, 6, {0x00, 39})[5], HidPp::kErrInvalidArgument);
    EXPECT_EQ(w.ask(4, 6, {0x09, 0xD9})[5], HidPp::kErrInvalidArgument);   // 2521
    EXPECT_EQ(w.hidpp.rotationDeg(), 2520);
}

// Freed effects are acknowledged; slots handed out stay within the wheel's 64 however many are uploaded.
TEST(HidPp, EffectSlotsStayInRangeAndFreesAreAcknowledged) {
    Wheel w;
    for (int i = 0; i < 200; ++i) {
        const std::uint8_t slot = w.ask(4, 2, {0x01})[4];
        EXPECT_GE(slot, 1);
        EXPECT_LE(slot, HidPp::kEffectSlots);
    }
    EXPECT_EQ(w.ask(4, 4, {3})[2], 4);
    bool freed = false;
    for (const std::string& line : w.lines) freed = freed || line.find("effect freed") != std::string::npos;
    EXPECT_TRUE(freed);
}

// Only sub ids 0x80..0x83 are HID++ 1.0 register access; the next one is a feature index this wheel lacks.
TEST(HidPp, Hidpp10RangeEndsAt0x83) {
    Wheel w;
    EXPECT_EQ(w.ask(0x80, 0)[2], 0x8F);
    EXPECT_EQ(w.ask(0x83, 0)[2], 0x8F);
    const auto past = w.ask(0x84, 0);
    EXPECT_EQ(past[2], 0xFF);
    EXPECT_EQ(past[5], HidPp::kErrInvalidFeatureIndex);
    EXPECT_EQ(w.ask(0x7F, 0)[5], HidPp::kErrInvalidFeatureIndex);
}

// Without a log or a settings callback (the driver may give none) every request is still answered and kept.
TEST(HidPp, WorksWithoutALogOrSettings) {
    HidPp hidpp{HidPp::Identity{"G923", 0xC26E, 1, 0x0A}, HidPp::Logger{}};
    const auto ask = [&hidpp](const std::uint8_t index, const std::uint8_t function,
                              const std::vector<std::uint8_t>& params) {
        std::vector<std::uint8_t> request(HidPp::kLongSize, 0);
        request[0] = HidPp::kLongId;
        request[1] = 0xFF;
        request[2] = index;
        request[3] = static_cast<std::uint8_t>(function << 4U | 1U);
        for (std::size_t i = 0; i < params.size(); ++i) request[4 + i] = params[i];
        return *hidpp.handle(request, 0);
    };
    EXPECT_EQ(ask(0, 0, {0x12, 0x34})[4], 0);   // not there, and no log to say so
    EXPECT_EQ(ask(0, 0, {0x81, 0x23})[4], 4);
    EXPECT_EQ(ask(4, 0, {})[4], HidPp::kEffectSlots);
    EXPECT_EQ(ask(4, 2, {1, 2})[4], 1);
    EXPECT_EQ(ask(4, 6, {0x01, 0x68})[2], 4);   // 360
    EXPECT_EQ(hidpp.rotationDeg(), 360);
    ask(4, 8, {0xFF, 0xFF, 0, 0});
    EXPECT_FLOAT_EQ(hidpp.gain(), 1.0F);
    EXPECT_EQ(ask(4, 9, {})[5], HidPp::kErrInvalidFunction);
    EXPECT_EQ(ask(0x81, 0, {})[2], 0x8F);
    EXPECT_EQ(hidpp.requests(), 8U);
}

// The descriptor's items: (prefix, value) in order.
std::vector<std::pair<std::uint8_t, std::uint32_t>> items(const std::span<const std::uint8_t> d) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> out;
    for (std::size_t i = 0; i < d.size();) {
        const std::uint8_t prefix = d[i];
        const std::size_t size = (prefix & 3U) == 3U ? 4 : (prefix & 3U);
        std::uint32_t value = 0;
        for (std::size_t b = 0; b < size && i + 1 + b < d.size(); ++b) value |= std::uint32_t{d[i + 1 + b]} << (8 * b);
        out.emplace_back(prefix, value);
        i += 1 + size;
    }
    return out;
}

// Report ID -> bits per kind (0x81 input, 0x91 output, 0xB1 feature), from the descriptor's items.
std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint32_t> reportBits(const std::span<const std::uint8_t> d) {
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint32_t> bits;
    std::uint32_t size = 0, count = 0;
    std::uint8_t id = 0;
    for (const auto& [prefix, value] : items(d)) {
        if (prefix == 0x75) size = value;
        if (prefix == 0x95) count = value;
        if (prefix == 0x85) id = static_cast<std::uint8_t>(value);
        if (prefix == 0x81 || prefix == 0x91 || prefix == 0xB1 || prefix == 0x92 || prefix == 0xB2) {
            bits[{id, static_cast<std::uint8_t>(prefix & 0xFCU)}] += size * count;
        }
    }
    return bits;
}

TEST(HidPpDescriptor, TheG923HasHidPpReportsAndItsPidMovedUp) {
    const VirtualWheel* g923 = virtualWheel("g923");
    ASSERT_NE(g923, nullptr);
    ASSERT_TRUE(g923->hidpp);
    const auto plain = g920ReportDescriptor(g923->buttons);
    const auto withHidPp = reportDescriptorFor(*g923, DescriptorFor::Linux);   // the games' pedal layout
    ASSERT_GT(withHidPp.size(), plain.size());
    const auto bits = reportBits(withHidPp);
    // HID++: 19 and 63 bytes each way, after the report ID.
    EXPECT_EQ((bits.at({0x11, 0x80})), 19U * 8U);
    EXPECT_EQ((bits.at({0x11, 0x90})), 19U * 8U);
    EXPECT_EQ((bits.at({0x12, 0x80})), 63U * 8U);
    EXPECT_EQ((bits.at({0x12, 0x90})), 63U * 8U);
    // The joystick's input report is as it was; every PID report is 0x10 higher, with the same size.
    const auto plainBits = reportBits(plain);
    EXPECT_EQ((bits.at({0x01, 0x80})), (plainBits.at({0x01, 0x80})));
    for (const auto& [key, size] : plainBits) {
        if (key.first == kG920InputReportId) continue;
        const std::pair<std::uint8_t, std::uint8_t> moved{static_cast<std::uint8_t>(key.first + kHidPpPidShift),
                                                          key.second};
        ASSERT_TRUE(bits.count(moved)) << int(key.first);
        EXPECT_EQ(bits.at(moved), size) << int(key.first);
    }
    // Item by item, the joystick part is the same but for the moved report IDs and the axes' unsigned range.
    const auto before = items(plain);
    const auto after = items(withHidPp);
    ASSERT_GT(after.size(), before.size());
    int axes = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const auto [prefix, value] = before[i];
        if (prefix == 0x85 && value != kG920InputReportId) {
            EXPECT_EQ(after[i], std::make_pair(prefix, value + kHidPpPidShift)) << i;
        } else if (prefix == 0x16 && value == 0x8000) {   // Logical Minimum -32768 (an axis)
            EXPECT_EQ(after[i], std::make_pair(std::uint8_t{0x15}, std::uint32_t{0})) << i;
            ++axes;
        } else if (prefix == 0x26 && value == 0x7FFF && axes > 0 && axes <= 4 && before[i - 1].first == 0x16) {
            EXPECT_EQ(after[i], std::make_pair(std::uint8_t{0x27}, std::uint32_t{0xFFFF})) << i;
        } else {
            EXPECT_EQ(after[i], before[i]) << i;
        }
    }
    EXPECT_EQ(axes, 4);
}

// On Windows the G923 reports its pedals as a real Logitech wheel does (brake Z, clutch Rz) for G HUB: the only
// difference from the Linux descriptor, two Usage items.
TEST(HidPpDescriptor, OnWindowsTheG923HasLogitechsPedalLayout) {
    const VirtualWheel& g923 = *virtualWheel("g923");
    ASSERT_TRUE(g923.logitechPedals);
    const auto windows = items(reportDescriptorFor(g923, DescriptorFor::Windows));
    const auto onLinux = items(reportDescriptorFor(g923, DescriptorFor::Linux));
    ASSERT_EQ(windows.size(), onLinux.size());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> changed;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (windows[i] != onLinux[i]) {
            EXPECT_EQ(windows[i].first, 0x09) << i;   // a Usage
            changed.emplace_back(onLinux[i].second, windows[i].second);
        }
    }
    // Rz -> Z, Slider -> Rz.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected = {{0x35, 0x32}, {0x36, 0x35}};
    EXPECT_EQ(changed, expected);
}

// Games still read the brake on Rz and the clutch on Slider: DirectInput's axis entries say which HID usage feeds
// each DirectInput axis (Z gets the Slider usage, which the wheel lacks, so nothing).
TEST(HidPpDescriptor, DirectInputAxisEntriesPutThePedalsBack) {
    const auto entries = directInputAxisEntries(*virtualWheel("g923"));
    ASSERT_EQ(entries.size(), 3U);
    const std::map<int, std::uint8_t> usageOf = {{entries[0].instance, entries[0].attributes[6]},
                                                 {entries[1].instance, entries[1].attributes[6]},
                                                 {entries[2].instance, entries[2].attributes[6]}};
    EXPECT_EQ(usageOf.at(5), 0x32);   // Rz <- HID Z (the brake)
    EXPECT_EQ(usageOf.at(6), 0x35);   // Slider <- HID Rz (the clutch)
    EXPECT_EQ(usageOf.at(2), 0x36);   // Z <- HID Slider (none)
    for (const auto& entry : entries) {
        EXPECT_EQ(entry.attributes[0], 0x00);
        EXPECT_EQ(entry.attributes[1], 0x01);   // DIDOI_ASPECTPOSITION
        EXPECT_EQ(entry.attributes[4], 0x01);   // Generic Desktop
    }
    for (const char* name : {"g920", "g25", "g27"}) EXPECT_TRUE(directInputAxisEntries(*virtualWheel(name)).empty());
}

// The G923's axes are unsigned (a real Logitech wheel's are; G HUB drew the signed ones half a turn off): the same
// state packs to the same bytes but for the top bit of each axis.
TEST(HidPpDescriptor, TheG923PacksItsAxesUnsigned) {
    const VirtualWheel& g923 = *virtualWheel("g923");
    ASSERT_TRUE(g923.unsignedAxes);
    TargetReport report = g920Neutral();   // centred, pedals released (65535)
    report.buttons = 1U << 23;            // Dial Enter
    const auto bytes = packInputFor(g923, report);
    EXPECT_EQ(bytes[1], 0x00);   // steering 32768: 0x8000, little-endian
    EXPECT_EQ(bytes[2], 0x80);
    EXPECT_EQ(bytes[3], 0xFF);   // throttle released: 65535
    EXPECT_EQ(bytes[4], 0xFF);
    report.steering = 0;          // full left
    EXPECT_EQ(packInputFor(g923, report)[2], 0x00);
    EXPECT_EQ(packInputFor(g923, report)[11], 0x80);   // button 23 still sent
    // The other wheels: as before (signed).
    for (const char* name : {"g920", "g25", "g27"}) {
        const VirtualWheel& wheel = *virtualWheel(name);
        EXPECT_FALSE(wheel.unsignedAxes) << name;
        EXPECT_EQ(packInputFor(wheel, report), packG920Input(report, wheel.buttons)) << name;
    }
}

TEST(HidPpDescriptor, WheelsWithoutHidPpKeepTheirDescriptor) {
    for (const char* name : {"g920", "g25", "g27"}) {
        const VirtualWheel* wheel = virtualWheel(name);
        ASSERT_NE(wheel, nullptr);
        EXPECT_FALSE(wheel->hidpp) << name;
        EXPECT_EQ(reportDescriptorFor(*wheel).data(), g920ReportDescriptor(wheel->buttons).data()) << name;
        EXPECT_EQ(routeReport(*wheel, 0x11).to, ReportRoute::To::Pid) << name;   // PID's Set Effect, as always
        EXPECT_EQ(routeReport(*wheel, 0x11).id, 0x11) << name;
        EXPECT_EQ(wirePidId(*wheel, 0x12), 0x12) << name;
    }
}

TEST(HidPpDescriptor, TheG923RoutesByReportId) {
    const VirtualWheel& g923 = *virtualWheel("g923");
    EXPECT_EQ(routeReport(g923, 0x11).to, ReportRoute::To::HidPp);
    EXPECT_EQ(routeReport(g923, 0x12).to, ReportRoute::To::HidPp);
    EXPECT_EQ(routeReport(g923, 0x21).to, ReportRoute::To::Pid);   // Set Effect
    EXPECT_EQ(routeReport(g923, 0x21).id, 0x11);
    EXPECT_EQ(routeReport(g923, 0x2D).id, 0x1D);   // Device Gain
    EXPECT_EQ(routeReport(g923, 0x15).to, ReportRoute::To::None);
    EXPECT_EQ(wirePidId(g923, 0x12), 0x22);   // Block Load
}

}  // namespace
}  // namespace wheelbri
