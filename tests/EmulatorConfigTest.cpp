#include "wheelemu/EmulatorConfig.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace wheelemu {
namespace {

EmulatorConfigResult load(const char* text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    return loadEmulatorConfig(parsed.document);
}

bool containsError(const EmulatorConfigResult& result, const std::string& fragment) {
    return std::any_of(result.errors.begin(), result.errors.end(),
                       [&](const std::string& error) { return error.find(fragment) != std::string::npos; });
}

TEST(EmulatorConfig, EmptyFileGivesDefaults) {
    const auto result = load("");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.config.sourceDeviceMatch, "346e");
    EXPECT_EQ(result.config.targetProfile, "g920");
    EXPECT_EQ(result.config.statePort, kDefaultStatePort);
    EXPECT_EQ(result.config.loopHz, 1000);
}

TEST(EmulatorConfig, ReadsEverySection) {
    const auto result = load(
        "[emulator]\nsource_device = R3\nstate_port = 50000\nffb_port = 50001\nloop_hz = 500\n"
        "[steering]\naxis = 0\nsource_rotation_deg = 540\ntarget_rotation_deg = 900\ndeadzone = 0.02\ninvert = yes\n"
        "[throttle]\naxis = 1\ninvert = true\n"
        "[brake]\naxis = 2\ndeadzone_low = 0.05\nsaturation_high = 0.8\ngamma = 1.8\n"
        "[clutch]\naxis = -1\n"
        "[buttons]\ntarget_0 = 7\ntarget_1 = -1\n"
        "[hat]\nup = 4\nright = 5\ndown = 6\nleft = 7\n"
        "[ffb]\nmaster_gain = 0.6\nmax_magnitude = 0.7\nmax_slew_per_second = 40\ninvert = true\n"
        "game_effects = false\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();

    const EmulatorConfig& config = result.config;
    EXPECT_EQ(config.sourceDeviceMatch, "R3");
    EXPECT_EQ(config.statePort, 50000);
    EXPECT_EQ(config.loopHz, 500);
    EXPECT_FLOAT_EQ(config.mapper.steering.sourceRotationDeg, 540.0F);
    EXPECT_TRUE(config.mapper.steering.invert);
    EXPECT_EQ(config.mapper.throttle.sourceAxis, 1);
    EXPECT_TRUE(config.mapper.throttle.invert);
    EXPECT_FLOAT_EQ(config.mapper.brake.gamma, 1.8F);
    EXPECT_EQ(config.mapper.clutch.sourceAxis, kUnmapped);
    EXPECT_EQ(config.mapper.buttonMap[0], 7);
    EXPECT_EQ(config.mapper.buttonMap[1], kUnmapped);
    EXPECT_EQ(config.mapper.buttonMap[2], 2);
    EXPECT_EQ(config.mapper.hatButtons.up, 4);
    EXPECT_EQ(config.mapper.hatButtons.left, 7);
    EXPECT_FLOAT_EQ(config.ffb.masterGain, 0.6F);
    EXPECT_FLOAT_EQ(config.ffb.maxSlewPerSecond, 40.0F);
    EXPECT_TRUE(config.ffbInvert);
    EXPECT_EQ(config.ffbPort, 50001);
    EXPECT_FALSE(config.gameFfb);
}

TEST(EmulatorConfig, ReadsTelemetry) {
    const auto defaults = load("");
    EXPECT_TRUE(defaults.config.telemetry.enabled);
    EXPECT_EQ(defaults.config.telemetry.rateHz, 50);

    const auto result = load("[telemetry]\nenabled = false\nrate_hz = 100\nmax_file_mb = 50\nonly_changes = false\nheartbeat_s = 5\nmin_change = 0.001\n"
                             "directory = D:/telemetry\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    const TelemetryConfig& telemetry = result.config.telemetry;
    EXPECT_FALSE(telemetry.enabled);
    EXPECT_EQ(telemetry.rateHz, 100);
    EXPECT_EQ(telemetry.maxFileMb, 50);
    EXPECT_FALSE(telemetry.onlyChanges);
    EXPECT_EQ(telemetry.heartbeatSeconds, 5);
    EXPECT_FLOAT_EQ(telemetry.minChange, 0.001F);
    EXPECT_EQ(telemetry.directory, "D:/telemetry");

    const auto bad = load("[telemetry]\nrate_hz = 0\nfolder = x\n");
    EXPECT_EQ(bad.config.telemetry.rateHz, 50);
    EXPECT_TRUE(containsError(bad, "[telemetry] rate_hz = '0'"));
    EXPECT_TRUE(containsError(bad, "[telemetry] unknown key 'folder'"));
}

TEST(EmulatorConfig, ReadsRetention) {
    const auto defaults = load("");
    EXPECT_EQ(defaults.config.retention.action, OldFilesAction::Delete);
    EXPECT_EQ(defaults.config.retention.olderThanDays, 7);
    EXPECT_EQ(defaults.config.retention.archiver, "auto");

    const auto result = load("[retention]\nold_files = archive\nolder_than_days = 14\narchiver = 7z\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.config.retention.action, OldFilesAction::Archive);
    EXPECT_EQ(result.config.retention.olderThanDays, 14);
    EXPECT_EQ(result.config.retention.archiver, "7z");

    const auto bad = load("[retention]\nold_files = shred\nolder_than_days = 0\narchiver = rar\n");
    EXPECT_EQ(bad.config.retention.action, OldFilesAction::Delete);
    EXPECT_TRUE(containsError(bad, "[retention] old_files = 'shred'"));
    EXPECT_TRUE(containsError(bad, "[retention] older_than_days = '0'"));
    EXPECT_TRUE(containsError(bad, "[retention] archiver = 'rar'"));
}

TEST(EmulatorConfig, RemovedLoggingKeysAreReported) {
    const auto result = load("[logging]\nmax_files = 3\n");
    EXPECT_TRUE(containsError(result, "[logging] unknown key 'max_files'"));
}

TEST(EmulatorConfig, InvalidValuesKeepDefaultsAndAreReported) {
    const auto result = load("[steering]\naxis = nine\ndeadzone = 0.9\n[ffb]\nmaster_gain = 2\n");
    EXPECT_EQ(result.config.mapper.steering.sourceAxis, 0);
    EXPECT_FLOAT_EQ(result.config.mapper.steering.deadzone, 0.0F);
    EXPECT_FLOAT_EQ(result.config.ffb.masterGain, 1.0F);
    EXPECT_TRUE(containsError(result, "[steering] axis = 'nine'"));
    EXPECT_TRUE(containsError(result, "[steering] deadzone = '0.9'"));
    EXPECT_TRUE(containsError(result, "[ffb] master_gain = '2'"));
}

TEST(EmulatorConfig, UnknownKeysAndSectionsAreReported) {
    const auto result = load("stray = 1\n[steering]\naxes = 0\n[telemtry]\nport = 1\n");
    EXPECT_TRUE(containsError(result, "[steering] unknown key 'axes'"));
    EXPECT_TRUE(containsError(result, "unknown section [telemtry]"));
    EXPECT_TRUE(containsError(result, "keys found before the first [section]"));
}

TEST(EmulatorConfig, ReadsLoggingSection) {
    const auto result = load(
        "[logging]\nlevel = debug\nfile = /tmp/we.log\nconsole = false\n"
        "flush_level = info\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    const logging::LogConfig& log = result.config.logging;
    EXPECT_EQ(log.level, "debug");
    EXPECT_EQ(log.filePath, "/tmp/we.log");
    EXPECT_FALSE(log.console);
    EXPECT_EQ(log.flushLevel, "info");
}

TEST(EmulatorConfig, InvalidLogLevelFallsBackToInfo) {
    const auto result = load("[logging]\nlevel = verbose\n");
    EXPECT_EQ(result.config.logging.level, "info");
    EXPECT_TRUE(containsError(result, "[logging] level = 'verbose'"));
}

TEST(EmulatorConfig, PedalSaturationMustExceedDeadzone) {
    const auto result = load("[brake]\ndeadzone_low = 0.6\nsaturation_high = 0.5\n");
    EXPECT_TRUE(containsError(result, "[brake] saturation_high must be greater than deadzone_low"));
}

TEST(EmulatorConfig, FfbPortMustDifferFromStatePort) {
    const auto result = load("[emulator]\nstate_port = 41330\n");
    EXPECT_TRUE(containsError(result, "ffb_port must differ"));
}

}  // namespace
}  // namespace wheelemu
