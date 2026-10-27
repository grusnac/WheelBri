#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/TrueForce.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

struct Log {
    std::vector<std::string> lines;
    TrueForceLog log{[this](LogLevel, const std::string& line) { lines.push_back(line); }};
};

std::vector<std::uint8_t> packet(const std::uint8_t type, const std::uint8_t samples = 0, const std::uint8_t flag = 0) {
    std::vector<std::uint8_t> p(kTrueForceReportSize, 0);
    p[0] = kTrueForceReportId;
    p[4] = type;
    p[5] = 7;
    p[10] = samples;
    p[11] = flag;
    return p;
}

TEST(TrueForce, OnlyTheG923HasTheInterface) {
    EXPECT_EQ(trueForceHardwareId(*virtualWheel("g923")), "wheelbri\\G923_TF");
    EXPECT_EQ(trueForceHardwareId(*virtualWheel("g920")), "");
    EXPECT_EQ(trueForceWheelForHardwareId("WHEELBRI\\g923_tf"), virtualWheel("g923"));
    EXPECT_EQ(trueForceWheelForHardwareId("wheelbri\\G923"), nullptr);
    EXPECT_EQ(trueForceWheelForHardwareId(""), nullptr);
    EXPECT_EQ(virtualWheelForHardwareId("wheelbri\\G923_TF"), nullptr);   // never taken for the wheel itself
}

TEST(TrueForce, DescriptorIsTheVendorCollectionWithReport1Of63Bytes) {
    const auto d = trueForceReportDescriptor();
    const std::vector<std::uint8_t> bytes(d.begin(), d.end());
    ASSERT_GE(bytes.size(), 10U);
    EXPECT_EQ((std::vector<std::uint8_t>{bytes.begin(), bytes.begin() + 6}),
              (std::vector<std::uint8_t>{0x06, 0xFD, 0xFF, 0x0A, 0x01, 0xFD}));
    EXPECT_EQ(bytes[8], 0x85);
    EXPECT_EQ(bytes[9], kTrueForceReportId);
    EXPECT_EQ(bytes.back(), 0xC0);
    int count63 = 0;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) count63 += bytes[i] == 0x95 && bytes[i + 1] == 0x3F;
    EXPECT_EQ(count63, 1);   // one Report Count for both Input and Output
}

TEST(TrueForce, ShowsTheFirstPacketsOfEachTypeThenOnlyCounts) {
    Log t;
    for (int i = 0; i < 5; ++i) t.log.output(packet(0x05), 0);
    ASSERT_EQ(t.lines.size(), 1U + TrueForceLog::kFirstShown);
    EXPECT_EQ(t.lines[0], "trueforce: first packet (something opened the TrueForce interface)");
    EXPECT_EQ(t.lines[1], "trueforce: 0x05 #1 (64 bytes): 01 00 00 00 05 07 (58 zeros)");
    t.log.output(packet(0x01, 4, 0x0d), 0);
    EXPECT_EQ(t.lines.back(), "trueforce: 0x01 #1 (64 bytes): 01 00 00 00 01 07 00 00 00 00 04 0d (52 zeros)");
}

TEST(TrueForce, SummarisesEveryTenSecondsWithTheStreamRateAndFlags) {
    Log t;
    for (std::int64_t ms = 0; ms < TrueForceLog::kSummaryMs; ++ms) t.log.output(packet(0x01, 4, 0x0d), ms);
    t.log.output(packet(0x01), TrueForceLog::kSummaryMs);
    // The test packets' zero bytes: cur and samples at full left.
    EXPECT_EQ(t.lines.back(), "trueforce: last 10 s: 0x01 x10001 (1000/s; bytes 10/11 00/00 x1 04/0d x10000; "
                              "cur -1.00..-1.00 avg|.| 1.00 flips 0; texture peak 1.00 in 40000 samples)");
    t.lines.clear();
    t.log.output(packet(0x04), TrueForceLog::kSummaryMs + 5);
    t.log.finish(TrueForceLog::kSummaryMs + 1005);
    ASSERT_GE(t.lines.size(), 2U);
    EXPECT_EQ(t.lines[t.lines.size() - 2], "trueforce: last 1 s: 0x04 x1");
    EXPECT_EQ(t.lines.back(), "trueforce: in all: 0x01 x10001 0x04 x1");
}

// A stream packet with force `cur` (-1..1) and its 13 window slots all `sample`, `fresh` of them new.
std::vector<std::uint8_t> stream(const double cur, const double sample, const std::uint8_t fresh) {
    auto p = packet(0x01, fresh, fresh > 0 ? 0x0d : 0x00);
    const auto put = [&p](const std::size_t at, const double v) {
        const auto raw = static_cast<std::uint16_t>(0x8000 + static_cast<int>(v * 32767));
        p[at] = static_cast<std::uint8_t>(raw & 0xFF);
        p[at + 1] = static_cast<std::uint8_t>(raw >> 8);
    };
    put(6, cur);
    put(8, cur);
    for (std::size_t slot = 0; slot < 13; ++slot) {
        put(12 + slot * 4, sample);
        put(14 + slot * 4, sample);
    }
    return p;
}

TEST(TrueForce, SummarisesTheStreamsForceAndTexture) {
    Log t;
    t.log.output(stream(0.0, 0.0, 0), 0);       // idle: no force, no samples
    t.log.output(stream(0.30, 0.05, 4), 1);
    t.log.output(stream(0.01, 0.0, 4), 2);      // within the dead band: no side
    t.log.output(stream(-0.50, -0.10, 4), 3);   // a flip
    t.log.output(stream(0.20, 0.02, 0), 4);     // another; its window not new: no samples
    t.log.output(stream(0.0, 0.9, 4), 5);       // new samples but the flag missing below
    auto noFlag = stream(0.0, 0.9, 4);
    noFlag[11] = 0;
    t.log.output(noFlag, 6);
    t.log.output(packet(0x04), TrueForceLog::kSummaryMs);
    const std::string& line = t.lines.back();
    EXPECT_NE(line.find("cur -0.50..0.30 avg|.| 0.14 flips 2; texture peak 0.90 in 16 samples"), std::string::npos)
        << line;
}

TEST(TrueForce, CountsOddReportsAndSaysWhenNothingCame) {
    Log quiet;
    quiet.log.finish(0);
    EXPECT_EQ(quiet.lines, std::vector<std::string>{"trueforce: nothing was sent to the TrueForce interface"});
    Log t;
    t.log.output(std::vector<std::uint8_t>{0x02, 0, 0, 0, 0x01, 0}, 0);   // another report ID
    t.log.output(std::vector<std::uint8_t>{0x01}, 0);                      // too short to have a type
    t.log.output(std::vector<std::uint8_t>{}, 0);
    EXPECT_EQ(t.lines[1], "trueforce: odd #1 (6 bytes): 02 00 00 00 01 00");
    t.log.finish(1);
    EXPECT_EQ(t.lines.back(), "trueforce: in all: odd x3");
}

struct Device {
    std::vector<std::string> lines;
    TrueForceDevice device{[this](LogLevel, const std::string& line) { lines.push_back(line); }};

    std::vector<TrueForceDevice::Report> send(std::vector<std::uint8_t> payload, const std::int64_t nowUs = 0) {
        std::vector<std::uint8_t> r{kTrueForceReportId, 0, 0, 0};
        r.insert(r.end(), payload.begin(), payload.end());
        r.resize(kTrueForceReportSize, 0);
        return device.output(r, nowUs);
    }
};

// The answer's bytes after the header, up to `count`.
std::vector<std::uint8_t> body(const TrueForceDevice::Report& r, const std::size_t count) {
    return {r.begin() + 4, r.begin() + 4 + static_cast<std::ptrdiff_t>(count)};
}

TEST(TrueForceDevice, AnswersAParameterWithItsCurrentValue) {
    Device d;
    const auto answers = d.send({0x05, 0x01, 0x00});
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers[0].size(), kTrueForceReportSize);
    EXPECT_EQ((std::vector<std::uint8_t>{answers[0].begin(), answers[0].begin() + 4}),
              (std::vector<std::uint8_t>{0x01, 0, 0, 0}));
    // As captured, checksum included: 05 01 00 09 00 00 00 00 40 b1 (2.0).
    EXPECT_EQ(body(answers[0], 11),
              (std::vector<std::uint8_t>{0x05, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x40, 0xB1, 0x00}));
    EXPECT_EQ(body(d.send({0x05, 0x05, 0x03})[0], 10),   // 65535.0
              (std::vector<std::uint8_t>{0x05, 0x05, 0x03, 0x09, 0x03, 0x00, 0xFF, 0x7F, 0x47, 0x22}));
    EXPECT_EQ(body(d.send({0x05, 0x02, 0x30})[0], 10),   // 3.0
              (std::vector<std::uint8_t>{0x05, 0x02, 0x30, 0x09, 0x30, 0x00, 0x00, 0x40, 0x40, 0x10}));
    EXPECT_EQ(body(d.send({0x05, 0x06, 0x7E})[0], 10),   // never seen: 0
              (std::vector<std::uint8_t>{0x05, 0x06, 0x7E, 0x09, 0x7E, 0x00, 0x00, 0x00, 0x00, 0xF0}));
}

TEST(TrueForceDevice, AnswersTheSlotQueryWithSixSlots) {
    Device d;
    const auto answers = d.send({0x07, 0x34, 0x00});
    ASSERT_EQ(answers.size(), 1U);
    EXPECT_EQ(body(answers[0], 22), (std::vector<std::uint8_t>{0x07, 0x34, 0x06, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01,
                                                               0x03, 0x01, 0x01, 0x04, 0x01, 0x01, 0x05, 0x01, 0x00,
                                                               0x06, 0x01, 0x01, 0x9F}));
}

TEST(TrueForceDevice, StartAndStopAnswerWithStatusAndSetTheSession) {
    Device d;
    EXPECT_FALSE(d.device.running());
    auto answers = d.send({0x03, 0x44, 0x00}, 10);
    ASSERT_EQ(answers.size(), 1U);
    const auto& s = answers[0];
    EXPECT_EQ(s[4], 0x02);
    EXPECT_EQ(s[5], 0x44);
    EXPECT_EQ(s[8], 0x03);
    EXPECT_EQ(s[9], 0x00);   // steering centred: signed 0
    EXPECT_EQ(s[10], 0x00);
    EXPECT_EQ(s[12], 0x00);
    EXPECT_EQ((std::vector<std::uint8_t>{s.begin() + 17, s.begin() + 33}),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x07, 0x00, 0x00, 0x1D, 0x95, 0x00,
                                         0x00, 0x1D, 0x95}));
    EXPECT_TRUE(d.device.running());
    EXPECT_EQ(d.send({0x04, 0x45, 0x00})[0][4], 0x02);
    EXPECT_FALSE(d.device.running());
    EXPECT_NE(std::find(d.lines.begin(), d.lines.end(), "trueforce: session started"), d.lines.end());
}

TEST(TrueForceDevice, SlotConfigAndRangeAnswerWithTheirStatusTypes) {
    Device d;
    auto slot = d.send({0x06, 0x36, 0x01, 0x01});
    ASSERT_EQ(slot.size(), 1U);
    EXPECT_EQ(slot[0][4], 0x10);
    EXPECT_EQ((std::vector<std::uint8_t>{slot[0].begin() + 17, slot[0].begin() + 21}),
              (std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}));
    auto range = d.send({0x0E, 0x46, 0x00, 0x00, 0xB4, 0x42});   // 90.0
    ASSERT_EQ(range.size(), 2U);
    EXPECT_EQ(range[0][4], 0x11);
    EXPECT_EQ(range[1][4], 0x14);
    EXPECT_EQ(range[1][5], 0x46);
    EXPECT_EQ((std::vector<std::uint8_t>{range[0].begin() + 17, range[0].begin() + 21}),
              (std::vector<std::uint8_t>{0x00, 0x00, 0xB4, 0x42}));
    EXPECT_NE(std::find(d.lines.begin(), d.lines.end(), "trueforce: operating range 90.0 degrees"), d.lines.end());
}

TEST(TrueForceDevice, StreamPacketsGetAStatusEachAndOthersNothing) {
    Device d;
    auto stream = d.send({0x01, 0x50, 0x00, 0x80, 0x00, 0x80, 0x04, 0x0D});
    ASSERT_EQ(stream.size(), 1U);
    EXPECT_EQ(stream[0][4], 0x02);
    EXPECT_EQ(stream[0][5], 0x50);
    EXPECT_TRUE(d.send({0x09, 0x51}).empty());
    EXPECT_TRUE(d.send({0x0C, 0x52}).empty());
    EXPECT_TRUE(d.send({0x0B, 0x53}).empty());
    EXPECT_TRUE(d.device.output(std::vector<std::uint8_t>{0x02, 0, 0, 0, 0x05, 1}, 0).empty());   // not report 1
    EXPECT_TRUE(d.device.output(std::vector<std::uint8_t>{0x01, 0}, 0).empty());
}

TEST(TrueForceDevice, SteeringReportsOnlyInASessionAtMostOnceAMillisecond) {
    Device d;
    EXPECT_FALSE(d.device.steering(1000, 0));   // no session: nothing, but the position is kept
    const auto start = d.send({0x03, 0x10}, 5000);
    ASSERT_EQ(start.size(), 1U);
    EXPECT_EQ(start[0][9], 1000 & 0xFF);         // the kept position
    EXPECT_EQ(start[0][11], 0x00);                // before it: centred 0x8000
    EXPECT_EQ(start[0][12], 0x00);
    EXPECT_EQ(start[0][13], 5000 & 0xFF);         // the clock, in microseconds
    EXPECT_EQ(start[0][14], 5000 >> 8);
    EXPECT_FALSE(d.device.steering(2000, 5900));   // the start's status went out under 1 ms ago
    const auto s = d.device.steering(70000, 6000);   // clamped
    ASSERT_TRUE(s);
    EXPECT_EQ((*s)[4], 0x02);
    EXPECT_EQ((*s)[5], 0x10);                     // the host's last sequence
    EXPECT_EQ((*s)[9], 0xFF);
    EXPECT_EQ((*s)[10], 0x7F);                    // signed: 65535 - 32768
    EXPECT_EQ((*s)[11], 1000 & 0xFF);             // the one in the status before (2000 never went out)
    EXPECT_EQ((*s)[12], (1000 >> 8) | 0x80);
    EXPECT_FALSE(d.device.steering(-5, 6500));
    const auto later = d.device.steering(-5, 7000);
    ASSERT_TRUE(later);
    EXPECT_EQ((*later)[9], 0x00);
    EXPECT_EQ((*later)[10], 0x80);                // 0: full left, -32768
    EXPECT_EQ((*later)[11], 0xFF);
    EXPECT_EQ((*later)[12], 0x7F);
}

// Every status ends in its checksum at 33: the bytes from the type through it sum to 0.
TEST(TrueForceDevice, StatusReportsCarryTheirChecksum) {
    Device d;
    std::vector<TrueForceDevice::Report> all = d.send({0x03, 0x44}, 3000);
    for (auto& r : d.send({0x06, 0x36, 0x01, 0x01}, 4000)) all.push_back(r);
    for (auto& r : d.send({0x0E, 0x46, 0x00, 0x00, 0xB4, 0x42}, 5000)) all.push_back(r);
    all.push_back(*d.device.steering(12345, 9000));
    ASSERT_EQ(all.size(), 5U);
    for (const auto& r : all) {
        unsigned sum = 0;
        for (std::size_t i = 4; i <= 33; ++i) sum += r[i];
        EXPECT_EQ(sum & 0xFFU, 0U) << "type " << int(r[4]);
        EXPECT_EQ(std::count(r.begin() + 34, r.end(), 0), static_cast<std::ptrdiff_t>(kTrueForceReportSize - 34));
    }
}

}  // namespace
}  // namespace wheelbri
