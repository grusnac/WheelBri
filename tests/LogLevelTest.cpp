// LogLevel: the app's Level button (warn -> info -> debug -> trace -> warn) and its wire packet.

#include "wheelbri/LogLevel.hpp"
#include "wheelbri/LogLevelPacket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace wheelbri {
namespace {

constexpr std::array<LogLevel, 7> kAll = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn,
                                          LogLevel::Error, LogLevel::Critical, LogLevel::Off};

TEST(LogLevel, NamesAndParsingRoundTrip) {
    for (const LogLevel level : kAll) EXPECT_EQ(parseLogLevel(logLevelName(level)), level);
    EXPECT_EQ(parseLogLevel("warning"), LogLevel::Warn);
    EXPECT_FALSE(parseLogLevel("").has_value());
    EXPECT_FALSE(parseLogLevel("WARN").has_value());   // as spdlog and the configs write them
    EXPECT_FALSE(parseLogLevel("verbose").has_value());
    EXPECT_STREQ(logLevelName(static_cast<LogLevel>(99)), "warn");
}

TEST(LogLevel, TheButtonGoesWarnInfoDebugTraceAndBack) {
    EXPECT_EQ(nextAppLogLevel(LogLevel::Warn), LogLevel::Info);
    EXPECT_EQ(nextAppLogLevel(LogLevel::Info), LogLevel::Debug);
    EXPECT_EQ(nextAppLogLevel(LogLevel::Debug), LogLevel::Trace);
    EXPECT_EQ(nextAppLogLevel(LogLevel::Trace), LogLevel::Warn);
    // Quieter than warn is warn, so the next is info.
    EXPECT_EQ(nextAppLogLevel(LogLevel::Error), LogLevel::Info);
    EXPECT_EQ(nextAppLogLevel(LogLevel::Off), LogLevel::Info);
    LogLevel level = kDefaultAppLogLevel;
    for (int click = 0; click < 4; ++click) level = nextAppLogLevel(level);
    EXPECT_EQ(level, LogLevel::Warn);   // four clicks: round
}

TEST(LogLevel, WarningsAndErrorsCannotBeTurnedOff) {
    EXPECT_EQ(clampToApp(LogLevel::Error), LogLevel::Warn);
    EXPECT_EQ(clampToApp(LogLevel::Critical), LogLevel::Warn);
    EXPECT_EQ(clampToApp(LogLevel::Off), LogLevel::Warn);
    EXPECT_EQ(clampToApp(LogLevel::Trace), LogLevel::Trace);
    EXPECT_EQ(appLogLevel("off"), LogLevel::Warn);
    EXPECT_EQ(appLogLevel("error"), LogLevel::Warn);
    EXPECT_EQ(appLogLevel("debug"), LogLevel::Debug);
    EXPECT_EQ(appLogLevel(""), LogLevel::Warn);   // nothing stored yet: the default
    EXPECT_EQ(appLogLevel("loud"), LogLevel::Warn);
}

TEST(LogLevel, AThresholdLetsItsLevelAndEveryHigherOneThrough) {
    EXPECT_TRUE(logLevelAllows(LogLevel::Warn, LogLevel::Warn));
    EXPECT_TRUE(logLevelAllows(LogLevel::Warn, LogLevel::Error));
    EXPECT_TRUE(logLevelAllows(LogLevel::Warn, LogLevel::Critical));
    EXPECT_FALSE(logLevelAllows(LogLevel::Warn, LogLevel::Info));
    EXPECT_TRUE(logLevelAllows(LogLevel::Trace, LogLevel::Trace));
    EXPECT_FALSE(logLevelAllows(LogLevel::Debug, LogLevel::Trace));
    EXPECT_FALSE(logLevelAllows(LogLevel::Trace, LogLevel::Off));   // "off" is not a line's level
}

TEST(LogLevelPacket, RoundTripsEveryAppLevel) {
    for (const LogLevel level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn}) {
        const auto bytes = encode(LogLevelPacket{level});
        const auto decoded = decodeLogLevelPacket(bytes);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->level, level);
    }
    const auto bytes = encode(LogLevelPacket{LogLevel::Debug});
    EXPECT_EQ(bytes, (std::array<std::uint8_t, 8>{'W', 'E', 'L', 'V', 1, 1, 0, 0}));
}

TEST(LogLevelPacket, AQuietLevelArrivesAsWarn) {
    const auto decoded = decodeLogLevelPacket(encode(LogLevelPacket{LogLevel::Off}));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->level, LogLevel::Warn);
}

TEST(LogLevelPacket, AnythingElseIsRefused) {
    const auto good = encode(LogLevelPacket{LogLevel::Info});
    EXPECT_FALSE(decodeLogLevelPacket(std::span<const std::uint8_t>(good.data(), 7)).has_value());   // short
    std::array<std::uint8_t, 9> longer{};
    std::copy(good.begin(), good.end(), longer.begin());
    EXPECT_FALSE(decodeLogLevelPacket(longer).has_value());
    for (const std::size_t at : {0U, 1U, 2U, 3U}) {   // the magic
        auto bad = good;
        bad[at] ^= 0x20U;
        EXPECT_FALSE(decodeLogLevelPacket(bad).has_value()) << at;
    }
    auto version = good;
    version[4] = 2;
    EXPECT_FALSE(decodeLogLevelPacket(version).has_value());
    auto level = good;
    level[5] = 7;   // no such level
    EXPECT_FALSE(decodeLogLevelPacket(level).has_value());
    auto reserved = good;
    reserved[6] = 1;
    EXPECT_FALSE(decodeLogLevelPacket(reserved).has_value());
    reserved = good;
    reserved[7] = 1;
    EXPECT_FALSE(decodeLogLevelPacket(reserved).has_value());
    // A state packet is not a level.
    EXPECT_FALSE(decodeLogLevelPacket(std::array<std::uint8_t, 33>{'W', 'E', 'M', 'U', 1}).has_value());
}

TEST(LogLevelBeacon, FirstAtOnceThenEveryTwoSecondsAndAtOnceOnANewLevel) {
    LogLevelBeacon beacon;
    EXPECT_TRUE(beacon.due(5'000, LogLevel::Warn));   // the session's first pass
    EXPECT_FALSE(beacon.due(5'001, LogLevel::Warn));
    EXPECT_FALSE(beacon.due(6'999, LogLevel::Warn));
    EXPECT_TRUE(beacon.due(7'000, LogLevel::Warn));   // kRepeatMs later: a game started meanwhile hears it
    EXPECT_TRUE(beacon.due(7'010, LogLevel::Debug));   // the Level button: at once
    EXPECT_FALSE(beacon.due(7'020, LogLevel::Debug));
    EXPECT_TRUE(beacon.due(100, LogLevel::Debug));   // a clock that went back: sent rather than never again
    EXPECT_FALSE(beacon.due(200, LogLevel::Debug));
}

}  // namespace
}  // namespace wheelbri
