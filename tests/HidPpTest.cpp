#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/HidPp.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
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
    const auto plain = wheelReportDescriptor(g923->buttons);
    const auto withHidPp = reportDescriptorFor(*g923);   // the games' pedal layout
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
        if (key.first == kWheelInputReportId) continue;
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
        if (prefix == 0x85 && value != kWheelInputReportId) {
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

// The G923 reports the games' axes on Windows as on Linux (throttle Y, brake Rz, clutch Slider, as the G920's): no HID
// Z, which DirectInput relabels on a car controller (in ACC gas was the clutch, brake the throttle, clutch the brake
// while the G923 had Logitech's brake Z and clutch Rz, 2026-10-21).
TEST(HidPpDescriptor, TheG923HasTheGamesAxesOnWindows) {
    const VirtualWheel& g923 = *virtualWheel("g923");
    const auto windows = reportDescriptorFor(g923);
    std::vector<std::uint32_t> usages;
    for (const auto& [prefix, value] : items(windows)) {
        if (prefix == 0x09 && usages.size() < 8) usages.push_back(value);   // the first Usages: the joystick's axes
    }
    ASSERT_GE(usages.size(), 5U);
    const std::vector<std::uint32_t> axes(usages.begin() + 1, usages.begin() + 5);   // after Usage Joystick
    EXPECT_EQ(axes, (std::vector<std::uint32_t>{0x30, 0x31, 0x35, 0x36}));          // X, Y, Rz, Slider
    EXPECT_EQ(directInputOemData(g923)[0], kJoyHwsHasPov | kJoyHwsIsCarCtrl);   // as the G920: harmless without Z
}

// The G923's axes are unsigned (a real Logitech wheel's are; G HUB drew the signed ones half a turn off): the same
// state packs to the same bytes but for the top bit of each axis.
TEST(HidPpDescriptor, TheG923PacksItsAxesUnsigned) {
    const VirtualWheel& g923 = *virtualWheel("g923");
    ASSERT_TRUE(g923.unsignedAxes);
    TargetReport report = neutralReport();   // centred, pedals released (65535)
    report.buttons = 1U << 23;            // Dial Enter
    const auto bytes = packInputFor(g923, report);
    EXPECT_EQ(bytes[1], 0x00);   // steering 32768: 0x8000, little-endian
    EXPECT_EQ(bytes[2], 0x80);
    EXPECT_EQ(bytes[3], 0xFF);   // throttle released: 65535
    EXPECT_EQ(bytes[4], 0xFF);
    report.steering = 0;          // full left
    EXPECT_EQ(packInputFor(g923, report)[2], 0x00);
    EXPECT_EQ(packInputFor(g923, report)[11], 0x80);   // button 23 still sent
    // The other wheels: as before (signed); the G25's and G27's fourth axis is their combined pedals.
    for (const char* name : {"g920", "g25", "g27"}) {
        const VirtualWheel& wheel = *virtualWheel(name);
        EXPECT_FALSE(wheel.unsignedAxes) << name;
        const auto plain = packWheelInput(report, wheel.buttons);
        const InputReport packed = packInputFor(wheel, report);
        EXPECT_TRUE(std::equal(plain.begin(), plain.begin() + 7, packed.bytes.begin())) << name;   // id, X, Y, Rz
    }
}

// The virtual G25 and G27, on Windows and Linux (VirtualWheel::combinedPedals): wheel X, throttle Y, brake Rz, the combined
// pedals on Slider 0 (in the middle at rest, down with the throttle, up with the brake) and the clutch on Slider 1, a
// fifth axis after them, as Logitech's driver gives them to games (GRID's G25 preset, ACC's G25 and G27 presets).
TEST(HidPpDescriptor, G25AndG27HaveCombinedPedalsAndTheClutchOnSlider1) {
    for (const char* name : {"g920", "g923"}) EXPECT_FALSE(virtualWheel(name)->combinedPedals) << name;
    for (const char* name : {"g25", "g27"}) {
        SCOPED_TRACE(name);
        const VirtualWheel& wheel = *virtualWheel(name);
        EXPECT_TRUE(wheel.combinedPedals);
        const auto windows = reportDescriptorFor(wheel);
        const auto plainDescriptor = wheelReportDescriptor(wheel.buttons);
        ASSERT_EQ(windows.size(), plainDescriptor.size() + 16);
        // The axes' usages in order: X, Y, Rz (brake), Slider (combined), Slider (clutch).
        std::vector<std::uint8_t> usages;
        for (std::size_t i = 8; i + 3 < 88; ++i) {   // past the joystick's own Usage Page, Usage, Collection, Report ID
            if (windows[i] == 0x05 && windows[i + 1] == 0x01 && windows[i + 2] == 0x09) {
                usages.push_back(windows[i + 3]);
            }
        }
        EXPECT_EQ(usages, (std::vector<std::uint8_t>{0x30, 0x31, 0x35, 0x36, 0x36}));
        // Everything before the fifth axis and after the axes as the wheel's own.
        EXPECT_TRUE(std::equal(plainDescriptor.begin(), plainDescriptor.begin() + 72, windows.begin()));
        EXPECT_TRUE(std::equal(plainDescriptor.begin() + 72, plainDescriptor.end(), windows.begin() + 88));

        TargetReport report = neutralReport();
        report.buttons = 1U << 18;
        report.clutch = 1000;
        InputReport packed = packInputFor(wheel, report);
        ASSERT_EQ(packed.size, kMaxInputReportSize);
        const auto plain = packWheelInput(report, wheel.buttons);
        EXPECT_TRUE(std::equal(plain.begin(), plain.begin() + 7, packed.bytes.begin()));     // id, wheel, Y, Rz
        EXPECT_EQ(packed[9], plain[7]);                                                         // the clutch: Slider 1
        EXPECT_EQ(packed[10], plain[8]);
        EXPECT_TRUE(std::equal(plain.begin() + 9, plain.end(), packed.bytes.begin() + 11));  // buttons, hat
        const auto slider0 = [&packed] {
            return static_cast<std::int16_t>(packed[7] | (packed[8] << 8));   // signed, as the other axes
        };
        EXPECT_EQ(slider0(), -1);            // released pedals: the middle (32767)
        report.throttle = 0;                 // full throttle
        packed = packInputFor(wheel, report);
        EXPECT_EQ(slider0(), -32768);        // the bottom
        report.throttle = 65535;
        report.brake = 0;                    // full brake
        packed = packInputFor(wheel, report);
        EXPECT_EQ(slider0(), 32767);         // the top
    }
}

TEST(HidPpDescriptor, WheelsWithoutHidPpKeepTheirDescriptor) {
    for (const char* name : {"g920", "g25", "g27"}) {
        const VirtualWheel* wheel = virtualWheel(name);
        ASSERT_NE(wheel, nullptr);
        EXPECT_FALSE(wheel->hidpp) << name;
        if (!wheel->combinedPedals) {   // the G25's and G27's two Sliders (G25AndG27HaveCombinedPedals...)
            EXPECT_EQ(reportDescriptorFor(*wheel).data(), wheelReportDescriptor(wheel->buttons).data()) << name;
        }
        EXPECT_EQ(routeReport(*wheel, 0x11).to, ReportRoute::To::Pid) << name;   // PID's Set Effect, as always
        EXPECT_EQ(routeReport(*wheel, 0x11).id, 0x11) << name;
        EXPECT_EQ(wirePidId(*wheel, 0x12), 0x12) << name;
    }
}

// DirectInput's OEMData: the G25 and G27 are no car controllers (with JOY_HWS_ISCARCTRL DirectInput gave GRID the
// G25's throttle as Slider 0 and its clutch as Y: directInputOemData), the others are; a hat and their buttons for all.
TEST(HidPpDescriptor, TheG25AndG27AreNoCarControllersForDirectInput) {
    const auto g25 = directInputOemData(*virtualWheel("g25"));
    const std::array<std::uint8_t, 8> expected = {kJoyHwsHasPov, 0, 0, 0, 19, 0, 0, 0};
    EXPECT_EQ(g25, expected);
    const auto g27 = directInputOemData(*virtualWheel("g27"));
    EXPECT_EQ(g27[0], kJoyHwsHasPov);
    EXPECT_EQ(g27[4], 23);
    for (const char* name : {"g920", "g923"}) {
        const VirtualWheel& wheel = *virtualWheel(name);
        const auto data = directInputOemData(wheel);
        EXPECT_EQ(data[0], kJoyHwsHasPov | kJoyHwsIsCarCtrl) << name;
        EXPECT_EQ(data[4], wheel.buttons) << name;
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

// The descriptor and the report follow the flag and the button count together: without combined pedals the G25 is
// the plain layout; with them on a wheel with other than the G25's or G27's buttons, too (none of their layout).
TEST(HidPpDescriptor, TheCombinedPedalsLayoutNeedsTheFlagAndTheButtons) {
    VirtualWheel wheel = *virtualWheel("g25");
    const TargetReport report = neutralReport();
    wheel.combinedPedals = false;
    const auto plainG25 = reportDescriptorFor(wheel);
    const auto g920 = wheelReportDescriptor(kG920Buttons);
    EXPECT_TRUE(std::equal(plainG25.begin(), plainG25.end(), g920.begin(), g920.end()));
    EXPECT_EQ(packInputFor(wheel, report).size, kWheelInputReportSize);
    EXPECT_EQ(directInputOemData(wheel)[0], kJoyHwsHasPov | kJoyHwsIsCarCtrl);

    wheel = *virtualWheel("g25");
    wheel.buttons = kG920Buttons + 2;
    const auto other = reportDescriptorFor(wheel);
    const auto plain = wheelReportDescriptor(wheel.buttons);
    EXPECT_TRUE(std::equal(other.begin(), other.end(), plain.begin(), plain.end()));
    EXPECT_EQ(packInputFor(wheel, report).size, packWheelInput(report, wheel.buttons).size());
}

}  // namespace
}  // namespace wheelbri
