// wheelbri-uhid's command line and its state feed (tools/uhid/UhidLoop), without /dev/uhid or a clock.

#include "UhidLoop.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wheelbri::uhid {
namespace {

// --- the command line ---

TEST(UhidOptions, DefaultsWithoutArguments) {
    const auto parsed = parseOptions({});
    ASSERT_TRUE(parsed.options);
    EXPECT_EQ(parsed.options->statePort, 41329);
    EXPECT_EQ(parsed.options->ffbPort, 41330);
    EXPECT_EQ(parsed.options->device, "/dev/uhid");
    EXPECT_TRUE(parsed.options->stopFile.empty());
}

TEST(UhidOptions, EveryOptionTakesItsValue) {
    const auto parsed = parseOptions({"--state-port", "1", "--ffb-port", "65535", "--stop-file", "/tmp/stop",
                                      "--device", "/dev/uhid2"});
    ASSERT_TRUE(parsed.options) << parsed.error;
    EXPECT_EQ(parsed.options->statePort, 1);
    EXPECT_EQ(parsed.options->ffbPort, 65535);
    EXPECT_EQ(parsed.options->stopFile, "/tmp/stop");
    EXPECT_EQ(parsed.options->device, "/dev/uhid2");
}

TEST(UhidOptions, HelpStartsNothingAndIsNoError) {
    for (const char* help : {"-h", "--help"}) {
        const auto parsed = parseOptions({"--state-port", "5000", help});
        EXPECT_FALSE(parsed.options) << help;
        EXPECT_TRUE(parsed.error.empty()) << help;
    }
}

TEST(UhidOptions, PortsAreDigitsInRange) {
    // atoi read "abc" as port 0 and 70000 as 4464.
    for (const char* bad : {"abc", "0", "65536", "70000", "-1", "+5", "41329x", " 41329", "", "123456", "4.5"}) {
        const auto parsed = parseOptions({"--state-port", bad});
        EXPECT_FALSE(parsed.options) << "'" << bad << "'";
        EXPECT_NE(parsed.error.find("not a port"), std::string::npos) << parsed.error;
    }
    EXPECT_TRUE(parseOptions({"--ffb-port", "00080"}).options);   // leading zeros are still a number in range
}

TEST(UhidOptions, BothPortsTheSameIsRefused) {
    const auto parsed = parseOptions({"--state-port", "5000", "--ffb-port", "5000"});
    EXPECT_FALSE(parsed.options);
    EXPECT_NE(parsed.error.find("both 5000"), std::string::npos) << parsed.error;
    EXPECT_FALSE(parseOptions({"--state-port", "41330"}).options);   // against the other's default
}

TEST(UhidOptions, UnknownOptionsAndMissingValuesAreRefused) {
    EXPECT_NE(parseOptions({"--verbose"}).error.find("unknown option --verbose"), std::string::npos);
    EXPECT_NE(parseOptions({"41329"}).error.find("unknown option"), std::string::npos);
    for (const char* option : {"--state-port", "--ffb-port", "--stop-file", "--device"}) {
        const auto parsed = parseOptions({option});
        EXPECT_FALSE(parsed.options) << option;
        EXPECT_NE(parsed.error.find("needs a value"), std::string::npos) << parsed.error;
    }
    EXPECT_FALSE(parseOptions({"--device", ""}).options);
    EXPECT_FALSE(parseOptions({"--stop-file", ""}).options);
}

// --- the state feed ---

struct Device {
    std::vector<uhid_event> written;
    UhidG920 g920{[this](const uhid_event& e) { written.push_back(e); return true; }, [](const FfbPacket&) {},
                  [](LogLevel, const std::string&) {}};

    Device() {
        g920.create();
        written.clear();
    }
    [[nodiscard]] std::size_t inputs() const {
        std::size_t n = 0;
        for (const auto& e : written) n += e.type == UHID_INPUT2 ? 1U : 0U;
        return n;
    }
    [[nodiscard]] std::vector<std::uint8_t> lastInput() const {
        for (auto it = written.rbegin(); it != written.rend(); ++it) {
            if (it->type == UHID_INPUT2) return {it->u.input2.data, it->u.input2.data + it->u.input2.size};
        }
        return {};
    }
};

StatePacket state(const std::int32_t steering, const std::uint32_t sequence = 1) {
    StatePacket p;
    p.sequence = sequence;
    p.report = g920Neutral();
    p.report.steering = steering;
    return p;
}

std::vector<std::uint8_t> packed(const TargetReport& r) {
    const auto a = packG920Input(r);
    return {a.begin(), a.end()};
}

TEST(UhidInputFeed, TheFirstStateIsSentAndSaid) {
    Device d;
    InputFeed feed(1000);
    EXPECT_EQ(feed.update(state(40000), 1000, d.g920), InputFeed::Change::Receiving);
    EXPECT_TRUE(feed.receiving());
    EXPECT_EQ(d.inputs(), 1U);
    EXPECT_EQ(d.lastInput(), packed(state(40000).report));
    EXPECT_EQ(feed.update(state(41000, 2), 1001, d.g920), InputFeed::Change::None);   // said once
    EXPECT_EQ(d.inputs(), 2U);                                                        // a change: sent at once
}

TEST(UhidInputFeed, AnUnchangedStateWaitsForTheKeepAlive) {
    Device d;
    InputFeed feed(0);
    feed.update(state(40000), 0, d.g920);
    feed.update(state(40000, 2), 50, d.g920);
    EXPECT_EQ(d.inputs(), 1U);   // same report within 100 ms: not again
    feed.update(state(40000, 3), 100, d.g920);
    EXPECT_EQ(d.inputs(), 2U);   // 100 ms since the last one: sent as a keep-alive
}

TEST(UhidInputFeed, WithoutStatesTheLastOneIsRepeatedEvery100Ms) {
    Device d;
    InputFeed feed(0);
    feed.update(state(40000), 0, d.g920);
    feed.update(std::nullopt, 99, d.g920);
    EXPECT_EQ(d.inputs(), 1U);
    feed.update(std::nullopt, 100, d.g920);
    EXPECT_EQ(d.inputs(), 2U);
    EXPECT_EQ(d.lastInput(), packed(state(40000).report));   // held, not neutral yet
}

TEST(UhidInputFeed, After1500MsWithoutStatesTheWheelGoesNeutralOnce) {
    Device d;
    InputFeed feed(0);
    feed.update(state(60000), 0, d.g920);
    for (std::int64_t t = 100; t <= 1500; t += 100) {
        EXPECT_EQ(feed.update(std::nullopt, t, d.g920), InputFeed::Change::None) << t;   // exactly 1500: held
    }
    EXPECT_EQ(d.lastInput(), packed(state(60000).report));
    EXPECT_EQ(feed.update(std::nullopt, 1501, d.g920), InputFeed::Change::Stale);
    EXPECT_FALSE(feed.receiving());
    EXPECT_EQ(d.lastInput(), packed(g920Neutral()));
    EXPECT_EQ(feed.update(std::nullopt, 1700, d.g920), InputFeed::Change::None);   // said once; keep-alive goes on
    EXPECT_EQ(d.lastInput(), packed(g920Neutral()));
    EXPECT_EQ(feed.update(state(20000, 9), 1800, d.g920), InputFeed::Change::Receiving);   // back
    EXPECT_EQ(d.lastInput(), packed(state(20000).report));
}

TEST(UhidInputFeed, NeverAnyStateIsNeverCalledStale) {
    Device d;
    InputFeed feed(0);
    for (std::int64_t t = 0; t <= 5000; t += 100) {
        EXPECT_EQ(feed.update(std::nullopt, t, d.g920), InputFeed::Change::None) << t;
    }
    EXPECT_EQ(d.lastInput(), packed(g920Neutral()));   // keep-alives of the neutral report
    EXPECT_GE(d.inputs(), 50U);
}

TEST(UhidInputFeed, TellsWhenItIsDueNext) {
    Device d;
    InputFeed feed(0);
    feed.update(state(40000), 0, d.g920);
    EXPECT_EQ(feed.msUntilDue(0), 100);   // the keep-alive
    EXPECT_EQ(feed.msUntilDue(60), 40);
    EXPECT_EQ(feed.msUntilDue(500), 0);   // overdue: never negative
    for (std::int64_t t = 100; t <= 1400; t += 100) feed.update(std::nullopt, t, d.g920);
    EXPECT_EQ(feed.msUntilDue(1450), 50);   // the next keep-alive at 1500 comes before going neutral at 1501
    feed.update(std::nullopt, 1500, d.g920);
    EXPECT_EQ(feed.msUntilDue(1500), 1);    // going neutral
    feed.update(std::nullopt, 1501, d.g920);
    EXPECT_EQ(feed.msUntilDue(1501), 100);  // neutral: only keep-alives
}

}  // namespace
}  // namespace wheelbri::uhid
