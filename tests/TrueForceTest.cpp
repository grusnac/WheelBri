#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/TrueForce.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

// Every packet but the stream; the stream's first kFirstShown, then one every kStreamShownMs.
TEST(TrueForce, ShowsEveryPacketButTheStreamWhichItSamples) {
    Log t;
    for (int i = 0; i < 5; ++i) t.log.output(packet(0x05), 0);
    ASSERT_EQ(t.lines.size(), 6U);
    EXPECT_EQ(t.lines[0], "trueforce: first packet (something opened the TrueForce interface)");
    EXPECT_EQ(t.lines[1], "trueforce: 0x05 #1 (64 bytes): 01 00 00 00 05 07 (58 zeros)");
    EXPECT_EQ(t.lines[5], "trueforce: 0x05 #5 (64 bytes): 01 00 00 00 05 07 (58 zeros)");
    t.log.output(packet(0x01, 4, 0x0d), 0);
    EXPECT_EQ(t.lines.back(), "trueforce: 0x01 #1 (64 bytes): 01 00 00 00 01 07 00 00 00 00 04 0d (52 zeros)");
    t.lines.clear();
    for (std::int64_t ms = 1; ms < TrueForceLog::kStreamShownMs; ++ms) t.log.output(packet(0x01, 0, 0), ms);
    ASSERT_EQ(t.lines.size(), 2U);   // #2 and #3 at once
    t.log.output(packet(0x01, 0, 0), TrueForceLog::kStreamShownMs - 1);
    EXPECT_EQ(t.lines.size(), 2U);
    t.log.output(packet(0x01, 0, 0), TrueForceLog::kStreamShownMs + 2);
    ASSERT_EQ(t.lines.size(), 3U);
    EXPECT_NE(t.lines.back().find("0x01 #4"), std::string::npos) << t.lines.back();
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

// Counts a turn as Logitech's DLL works them out: p4 * p5, in float32 as it multiplies them.
double countsPerTurn() {
    return static_cast<double>(TrueForceDevice::parameter(0x04) * TrueForceDevice::parameter(0x05));
}

// The angle and speed as Logitech's DLL works them out of a status: -(position - range / 2) / (p4 * p5) turns.
double angleDegrees(const TrueForceDevice::Report& r) {
    const auto position = static_cast<double>(r[6] | (r[7] << 8) | (r[8] << 16));
    const auto range = static_cast<double>(r[22] | (r[23] << 8) | (r[24] << 16));
    return -(position - std::floor(range / 2.0)) / countsPerTurn() * 360.0;
}

double speedDegreesPerSecond(const TrueForceDevice::Report& r) {
    const std::uint32_t raw = r[17] | (r[18] << 8) | (r[19] << 16) | (static_cast<std::uint32_t>(r[20]) << 24);
    return -static_cast<double>(static_cast<std::int64_t>(raw) - 0x80000000LL) / countsPerTurn() * 360.0;
}

// The answer's bytes after the header, up to `count`.
std::vector<std::uint8_t> body(const TrueForceDevice::Report& r, const std::size_t count) {
    return {r.begin() + 4, r.begin() + 4 + static_cast<std::ptrdiff_t>(count)};
}

// Parameter 0x36 negative switches the SDK's slot 3 on, which skips its KF reconstruction filter; the other slot
// switches (0x34, 0x35, 0x37, 0x38) stay off ("0 > NaN" is false), as on a real G923.
TEST(TrueForceDevice, SlotThreeIsOnSoTheSdkSkipsItsKfFilter) {
    EXPECT_LT(TrueForceDevice::parameter(0x36), 0.0F);
    for (const std::uint8_t index : {0x34, 0x35, 0x37, 0x38}) EXPECT_FALSE(0.0F > TrueForceDevice::parameter(index));
}

// Every parameter as a real G923 answers it (2026-11-01, captures/g923: ACC's set-up and the probe), as the 4 bytes
// on the wire. 0x36 is WheelBri's one change (above).
TEST(TrueForceDevice, ParametersAreTheRealG923s) {
    const std::array<std::uint32_t, 64> measured = {
        0x40000000U, 0x40000000U, 0x47000000U, 0x477FFF00U, 0x46800000U, 0x4182E8BAU, 0x3F19999AU, 0x3ECCCCCDU,
        0x3F267B5FU, 0x3F12AD5DU, 0x4182E8BAU, 0x3DF34D6AU, 0x4175BE0BU, 0x3F32B8C2U, 0xC0F5BE0BU, 0x40F5BE0BU,
        0x41500000U, 0x40000000U, 0x457A0000U, 0x41200000U, 0x44FA0000U, 0x43E10000U, 0x00000000U, 0x41F00000U,
        0x42200000U, 0x3F800000U, 0x3F000000U, 0x3F800000U, 0x3E99999AU, 0x40800000U, 0x40000000U, 0x477FFD00U,
        0x477D0100U, 0x3F800000U, 0x477F0000U, 0x40000000U, 0x00000000U, 0x3F800000U, 0x40800000U, 0x00000000U,
        0x00000000U, 0x3F800000U, 0x40400000U, 0x41800000U, 0x3C75C28FU, 0x3DCCCCCDU, 0x40400000U, 0xFFFFFFFFU,
        0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
        0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};
    for (std::size_t i = 0; i < measured.size(); ++i) {
        const auto index = static_cast<std::uint8_t>(i);
        if (index == 0x36) continue;
        EXPECT_EQ(TrueForceDevice::parameterBits(index), measured[i]) << "parameter 0x" << std::hex << i;
    }
    EXPECT_EQ(TrueForceDevice::parameterBits(0x40), 0U);
    // What the SDK makes of them (its shim log in ACC, 2026-11-01): peak torque 2 * p8 * p10 * p11 = 2.528 Nm, damping
    // max p9 = 0.573, 268101.8 counts a turn.
    EXPECT_NEAR(2.0 * TrueForceDevice::parameter(0x08) * TrueForceDevice::parameter(0x0A) *
                    TrueForceDevice::parameter(0x0B), 2.528444, 1e-4);
    EXPECT_NEAR(countsPerTurn(), 268101.8125, 1e-3);
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
    EXPECT_EQ(body(d.send({0x05, 0x02, 0x30})[0], 10),   // NaN, as captured (S2 frame 19): 30 09 30 ff ff ff ff 94
              (std::vector<std::uint8_t>{0x05, 0x02, 0x30, 0x09, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x94}));
    EXPECT_EQ(body(d.send({0x05, 0x06, 0x7E})[0], 10),   // never asked: 0
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
    // Centred: position 336821 = 0x0523B5 (half the range). [17..32] as a real G923 sends them (2026-11-01, S2 frame
    // 249): speed, 00, the count range 673643, the full scale 0.65 Nm twice.
    EXPECT_EQ((std::vector<std::uint8_t>{s.begin() + 6, s.begin() + 13}),
              (std::vector<std::uint8_t>{0xB5, 0x23, 0x05, 0x00, 0x80, 0x00, 0x80}));
    EXPECT_EQ((std::vector<std::uint8_t>{s.begin() + 17, s.begin() + 33}),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x80, 0x00, 0x6B, 0x47, 0x0A, 0x00, 0x00, 0x8A, 0x82, 0x00,
                                         0x00, 0x8A, 0x82}));
    EXPECT_NEAR(angleDegrees(s), 0.0, 1e-9);
    EXPECT_TRUE(d.device.running());
    EXPECT_EQ(d.send({0x04, 0x45, 0x00})[0][4], 0x02);
    EXPECT_FALSE(d.device.running());
    EXPECT_NE(std::find(d.lines.begin(), d.lines.end(), "trueforce: session started"), d.lines.end());
}

TEST(TrueForceDevice, SlotConfigAnswersWithStatusTenAndTheRangeWithNothing) {
    Device d;
    auto slot = d.send({0x06, 0x36, 0x01, 0x01});
    ASSERT_EQ(slot.size(), 1U);
    EXPECT_EQ(slot[0][4], 0x10);
    EXPECT_EQ((std::vector<std::uint8_t>{slot[0].begin() + 17, slot[0].begin() + 21}),
              (std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}));
    // A real G923 answers no operating range (ACC sent three, 2026-11-01); it is logged.
    EXPECT_TRUE(d.send({0x0E, 0x46, 0x00, 0x00, 0xB4, 0x42}).empty());   // 90.0
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

// The steering spans the wheel's rotation (360 here: 180 each way, + = left, as the DLL reads it), not the game's
// operating range.
TEST(TrueForceDevice, SteeringReportsOnlyInASessionAtMostOnceAMillisecond) {
    Device d;
    EXPECT_FALSE(d.device.steering(49152, 360, 0));   // no session: nothing, but the position is kept
    (void)d.send({0x0E, 0x01, 0x00, 0xC0, 0x28, 0x45}, 100);   // operating range 2700.0: not the wheel's
    const auto start = d.send({0x03, 0x10}, 5000);
    ASSERT_EQ(start.size(), 1U);
    EXPECT_NEAR(angleDegrees(start[0]), -90.0, 0.01);  // half right
    EXPECT_EQ(start[0][13], 5000 & 0xFF);               // the clock, in microseconds
    EXPECT_EQ(start[0][14], 5000 >> 8);
    EXPECT_FALSE(d.device.steering(32768, 0, 5900));       // the start's status went out under 1 ms ago
    const auto s = d.device.steering(70000, 0, 6000);      // clamped: full right
    ASSERT_TRUE(s);
    EXPECT_EQ((*s)[4], 0x02);
    EXPECT_EQ((*s)[5], 0x10);                           // the host's last sequence
    EXPECT_NEAR(angleDegrees(*s), -180.0, 0.01);
    EXPECT_FALSE(d.device.steering(-5, 0, 6500));
    const auto later = d.device.steering(-5, 0, 7000);     // full left
    ASSERT_TRUE(later);
    EXPECT_NEAR(angleDegrees(*later), 180.0, 0.01);
    const auto centre = d.device.steering(32768, 0, 8000);
    ASSERT_TRUE(centre);
    EXPECT_NEAR(angleDegrees(*centre), 0.0, 0.01);
}

// The speed comes from the steering samples, half new, half old: a status sent just after a sample does not divide
// the step by the few microseconds since the last status.
TEST(TrueForceDevice, SpeedComesFromTheSteeringSamples) {
    Device d;
    (void)d.device.steering(32768, 360, 0);
    (void)d.send({0x03, 0x10}, 1000);
    ASSERT_TRUE(d.device.steering(32768, 360, 2000));
    auto still = d.send({0x01, 0x11, 0x00, 0x80, 0x00, 0x80}, 3990);
    ASSERT_EQ(still.size(), 1U);
    EXPECT_NEAR(speedDegreesPerSecond(still[0]), 0.0, 1e-6);
    EXPECT_FALSE(d.device.steering(32950, 360, 4000));   // 1 degree right in 2 ms, 10 us after a status
    auto moving = d.send({0x01, 0x12, 0x00, 0x80, 0x00, 0x80}, 4030);
    ASSERT_EQ(moving.size(), 1U);
    EXPECT_NEAR(speedDegreesPerSecond(moving[0]), -250.0, 2.0);   // half of 500 deg/s, + = left
    EXPECT_NEAR(angleDegrees(moving[0]), -1.0, 0.01);
    ASSERT_TRUE(d.device.steering(32950, 360, 6000));   // no further: half again
    auto slowing = d.send({0x01, 0x13, 0x00, 0x80, 0x00, 0x80}, 7100);
    ASSERT_EQ(slowing.size(), 1U);
    EXPECT_NEAR(speedDegreesPerSecond(slowing[0]), -125.0, 1.0);
}

// Every status ends in its checksum at 33: the bytes from the type through it sum to 0.
TEST(TrueForceDevice, StatusReportsCarryTheirChecksum) {
    Device d;
    std::vector<TrueForceDevice::Report> all = d.send({0x03, 0x44}, 3000);
    for (auto& r : d.send({0x06, 0x36, 0x01, 0x01}, 4000)) all.push_back(r);
    all.push_back(*d.device.steering(12345, 0, 9000));
    ASSERT_EQ(all.size(), 3U);
    for (const auto& r : all) {
        unsigned sum = 0;
        for (std::size_t i = 4; i <= 33; ++i) sum += r[i];
        EXPECT_EQ(sum & 0xFFU, 0U) << "type " << int(r[4]);
        EXPECT_EQ(std::count(r.begin() + 34, r.end(), 0), static_cast<std::ptrdiff_t>(kTrueForceReportSize - 34));
    }
}

}  // namespace
}  // namespace wheelbri
