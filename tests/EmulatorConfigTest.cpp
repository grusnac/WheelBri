#include "wheelbri/EmulatorConfig.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace wheelbri {
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
    EXPECT_EQ(result.config.sourceDeviceMatch, "");   // the rig's wheel
    EXPECT_EQ(result.config.targetProfile, "g920");
    EXPECT_EQ(result.config.statePort, kDefaultStatePort);
    EXPECT_EQ(result.config.stateRateHz, 500);
}

TEST(EmulatorConfig, RejectsNotANumber) {
    // "nan" passes a range check (every comparison is false) and would switch the force cap off.
    const auto result = load("[ffb]\nmax_magnitude = nan\nmax_slew_per_second = inf\n[steering]\ndeadzone = -nan\n");
    EXPECT_TRUE(containsError(result, "max_magnitude"));
    EXPECT_TRUE(containsError(result, "max_slew_per_second"));
    EXPECT_TRUE(containsError(result, "deadzone"));
    EXPECT_FLOAT_EQ(result.config.ffb.maxMagnitude, FfbSafetyConfig{}.maxMagnitude);
}

TEST(EmulatorConfig, ReadsButtonLabels) {
    const auto result = load("[button_labels]\nbutton_35 = Start\nbutton_12 = L paddle\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.config.mapper.sourceButtonLabels[35], "Start");
    EXPECT_EQ(result.config.mapper.sourceButtonLabels[12], "L paddle");
    EXPECT_TRUE(result.config.mapper.sourceButtonLabels[0].empty());
}

TEST(EmulatorConfig, RejectsBadButtonLabels) {
    const auto result = load("[button_labels]\nbutton_64 = Nope\nbutton_1 = Much too long a name\n");
    EXPECT_TRUE(containsError(result, "[button_labels] unknown key 'button_64'"));
    EXPECT_TRUE(containsError(result, "button_1 = 'Much too long a name': at most 12 characters"));
    EXPECT_EQ(result.config.mapper.sourceButtonLabels[1], "Much too lon");
}

TEST(EmulatorConfig, ReadsAxisLabels) {
    const auto result = load("[axis_labels]\naxis_0 = Wheel\naxis_6 = Clutch pedal\naxis_8 = Nope\n");
    EXPECT_EQ(result.config.mapper.sourceAxisLabels[0], "Wheel");
    EXPECT_EQ(result.config.mapper.sourceAxisLabels[6], "Clutch pedal");
    EXPECT_TRUE(result.config.mapper.sourceAxisLabels[1].empty());
    EXPECT_TRUE(containsError(result, "[axis_labels] unknown key 'axis_8'"));
}

TEST(EmulatorConfig, TheFixedLoopsOldKeysStillLoadAndAreIgnored) {
    // Users' own configs, saved before the fixed loop went (2026-10-07), still have them: an unknown key would stop
    // every session.
    const auto result = load("[emulator]\nloop_mode = fixed\nloop_hz = 500\nstate_rate_hz = 250\n");
    EXPECT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.config.stateRateHz, 250);
    EXPECT_TRUE(load("[emulator]\nloop_mode = anything\n").errors.empty());
}

TEST(EmulatorConfig, ReadsEverySection) {
    const auto result = load(
        "[emulator]\nsource_device = Racing\nstate_port = 50000\nffb_port = 50001\nstate_rate_hz = 250\n"
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
    EXPECT_EQ(config.sourceDeviceMatch, "Racing");
    EXPECT_EQ(config.statePort, 50000);
    EXPECT_EQ(config.stateRateHz, 250);
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
    EXPECT_EQ(defaults.config.retention.olderThanDays, 2);
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

TEST(EmulatorConfig, InvalidLogLevelFallsBackToWarn) {
    const auto result = load("[logging]\nlevel = verbose\n");
    EXPECT_EQ(result.config.logging.level, "warn");
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

TEST(EmulatorConfig, ReadsSourcesOnAccessories) {
    const auto result = load(
        "[throttle]\naxis = 044f:b696/1\n[brake]\naxis = 044F:B696/2\n"
        "[buttons]\ntarget_4 = 1234:1/3\n[hat]\nup = 1234:0001/0\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    const MapperConfig& mapper = result.config.mapper;
    EXPECT_EQ(mapper.accessoryCount(), 2U);
    EXPECT_EQ(mapper.accessories[0], (DeviceId{0x044F, 0xB696}));
    EXPECT_EQ(mapper.accessories[1], (DeviceId{0x1234, 0x0001}));
    EXPECT_EQ(mapper.throttle.sourceDevice, 1);
    EXPECT_EQ(mapper.throttle.sourceAxis, 1);
    EXPECT_EQ(mapper.brake.sourceDevice, 1);   // the same pedals, one device
    EXPECT_EQ(mapper.buttonDevice[4], 2);
    EXPECT_EQ(mapper.buttonMap[4], 3);
    EXPECT_EQ(mapper.buttonDevice[5], 0);
    EXPECT_EQ(mapper.hatButtons.devices[0], 2);
    EXPECT_EQ(mapper.steering.sourceDevice, 0);
}

TEST(EmulatorConfig, RejectsBadSourcesAndTooManyAccessories) {
    const auto bad = load("[throttle]\naxis = 044f:b696/9\n[brake]\naxis = pedals/1\n[buttons]\ntarget_0 = 1:2/-1\n");
    EXPECT_TRUE(containsError(bad, "[throttle] axis"));
    EXPECT_TRUE(containsError(bad, "[brake] axis"));
    EXPECT_TRUE(containsError(bad, "[buttons] target_0"));
    const auto many = load("[buttons]\ntarget_0 = 1:1/0\ntarget_1 = 1:2/0\ntarget_2 = 1:3/0\ntarget_3 = 1:4/0\n");
    EXPECT_TRUE(containsError(many, "at most 3 accessories"));
}

}  // namespace
}  // namespace wheelbri
