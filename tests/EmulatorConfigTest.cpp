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

// from_rig: the inputs with no line follow the Rig (AutoMap.hpp), nothing is mapped until it fills them.
TEST(EmulatorConfig, ReadsFromRig) {
    EXPECT_FALSE(load("").config.fromRig);
    const auto result =
        load("[emulator]\nfrom_rig = true\n[brake]\naxis = 3\n[buttons]\ntarget_2 = -1\n[hat]\nleft = 9\n");
    EXPECT_TRUE(result.errors.empty());
    const EmulatorConfig& config = result.config;
    EXPECT_TRUE(config.fromRig);
    EXPECT_EQ(config.autoInputs.axes, (std::array<bool, 4>{true, true, false, true}));
    EXPECT_EQ(config.autoInputs.hat, (std::array<bool, 4>{true, true, true, false}));
    EXPECT_TRUE(config.autoInputs.buttons[0]);
    EXPECT_FALSE(config.autoInputs.buttons[2]);
    EXPECT_EQ(config.mapper.steering.sourceAxis, kUnmapped);
    EXPECT_EQ(config.mapper.brake.sourceAxis, 3);
    EXPECT_EQ(config.mapper.buttonMap[0], kUnmapped);
    EXPECT_EQ(config.mapper.hatButtons.left, 9);
    EXPECT_TRUE(containsError(load("[emulator]\nfrom_rig = maybe\n"), "from_rig"));
}

// trueforce_probe: off unless asked for.
TEST(EmulatorConfig, ReadsTheTrueForceProbe) {
    EXPECT_FALSE(load("").config.trueForceProbe);
    const auto result = load("[emulator]\ntarget_profile = g923\ntrueforce_probe = true\n");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.config.trueForceProbe);
    EXPECT_TRUE(containsError(load("[emulator]\ntrueforce_probe = 2\n"), "trueforce_probe"));
}

TEST(EmulatorConfig, ReadsTheG25Target) {
    const auto result = load("[emulator]\ntarget_profile = g25\n");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.config.targetProfile, "g25");
}

TEST(EmulatorConfig, RejectsNotANumber) {
    // "nan" passes a range check (every comparison is false) and would switch the force cap off.
    const auto result = load("[ffb]\nmax_magnitude = nan\nmax_slew_per_second = inf\n[brake]\ngamma = -nan\n");
    EXPECT_TRUE(containsError(result, "max_magnitude"));
    EXPECT_TRUE(containsError(result, "max_slew_per_second"));
    EXPECT_TRUE(containsError(result, "gamma"));
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
        "[ffb]\nmaster_gain = 0.6\nmax_magnitude = 0.7\nmax_slew_per_second = 40\nfrozen_force_s = 3.5\ninvert = true\n"
        "game_effects = false\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();

    const EmulatorConfig& config = result.config;
    EXPECT_EQ(config.sourceDeviceMatch, "Racing");
    EXPECT_EQ(config.statePort, 50000);
    EXPECT_EQ(config.stateRateHz, 250);
    EXPECT_FLOAT_EQ(config.mapper.steering.targetRotationDeg, 900.0F);   // source_rotation_deg: read and ignored
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
    EXPECT_FLOAT_EQ(config.ffb.frozenForceSeconds, 3.5F);
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
    const auto result = load("[steering]\naxis = nine\ntarget_rotation_deg = 5\n[ffb]\nmaster_gain = 2\n");
    EXPECT_EQ(result.config.mapper.steering.sourceAxis, 0);
    EXPECT_FLOAT_EQ(result.config.mapper.steering.targetRotationDeg, SteeringConfig{}.targetRotationDeg);
    EXPECT_FLOAT_EQ(result.config.ffb.masterGain, 1.0F);
    EXPECT_TRUE(containsError(result, "[steering] axis = 'nine'"));
    EXPECT_TRUE(containsError(result, "[steering] target_rotation_deg = '5'"));
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

// Dead zones are gone (the devices' own software sets them): an older config's lines load, whatever they say, and
// change nothing.
TEST(EmulatorConfig, OldDeadzonesLoadAndAreIgnored) {
    const auto result = load("[steering]\ndeadzone = 0.9\n[throttle]\ndeadzone_low = 0.6\nsaturation_high = 0.5\n"
                             "[brake]\ndeadzone_low = lots\n[clutch]\nsaturation_high = 0.98\n");
    EXPECT_TRUE(result.errors.empty()) << result.errors.front();
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

TEST(EmulatorConfig, ButtonSecondSources) {
    const auto parsed = IniDocument::parse(
        "[buttons]\ntarget_2_or = 30b7:1001/axis 3 inverted > 40%\ntarget_0_or = 20\ntarget_1_or = -1\n"
        "target_3_or = axis 9\n");
    const EmulatorConfigResult result = loadEmulatorConfig(parsed.document);
    ASSERT_EQ(result.errors.size(), 1U);   // axis 9: no such axis
    EXPECT_NE(result.errors[0].find("target_3_or"), std::string::npos) << result.errors[0];
    const MapperConfig& mapper = result.config.mapper;
    EXPECT_EQ(mapper.buttonSecondary[2][0], (ButtonSource{1, 3, true, true, 0.4F}));
    EXPECT_EQ(mapper.accessories[0], (DeviceId{0x30B7, 0x1001}));
    EXPECT_EQ(mapper.buttonSecondary[0][0], (ButtonSource{0, 20, false, false, 0.5F}));
    EXPECT_FALSE(mapper.buttonSecondary[1][0].mapped());
    EXPECT_EQ(mapper.buttonMap[2], 2);   // the first sources as they were
}

TEST(EmulatorConfig, ButtonSecondaryLists) {
    const auto parsed = IniDocument::parse(
        "[buttons]\ntarget_2_or = 20, 30b7:1001/4, 30b7:1001/axis 3 > 60%\n"
        "target_0_or = 1, 2, 3, 4, 5, 6, 7, 8, 9\ntarget_1_or = 4, , 5\ntarget_3_or = 64\n");
    const EmulatorConfigResult result = loadEmulatorConfig(parsed.document);
    ASSERT_EQ(result.errors.size(), 3U);
    EXPECT_NE(result.errors[0].find("target_0_or"), std::string::npos) << result.errors[0];
    EXPECT_NE(result.errors[0].find("at most 8"), std::string::npos) << result.errors[0];
    const MapperConfig& mapper = result.config.mapper;
    EXPECT_EQ(mapper.buttonSecondary[2][0], (ButtonSource{0, 20, false, false, 0.5F}));
    EXPECT_EQ(mapper.buttonSecondary[2][1], (ButtonSource{1, 4, false, false, 0.5F}));
    EXPECT_EQ(mapper.buttonSecondary[2][2], (ButtonSource{1, 3, true, false, 0.6F}));
    EXPECT_FALSE(mapper.buttonSecondary[2][3].mapped());
    EXPECT_FALSE(mapper.buttonSecondary[0][0].mapped());   // refused: none
}

// Numbers as a hand edit may leave them: trailing junk, a sign where none fits, too big for an int, an exponent.
// Each is reported and the default kept.
TEST(EmulatorConfig, RejectsMalformedNumbers) {
    const auto result = load("[emulator]\nstate_port = 41329x\nffb_port = 99999999999999999999\n"
                             "state_rate_hz = 1e3\n[ffb]\nmaster_gain = 0.5.1\nmax_magnitude = -0.1\n"
                             "max_slew_per_second = 1000.5\n[telemetry]\nrate_hz = 0\nmin_change = 0.05 \n");
    for (const char* key : {"state_port = '41329x'", "ffb_port = '99999999999999999999'", "state_rate_hz = '1e3'",
                            "master_gain = '0.5.1'", "max_magnitude = '-0.1'", "max_slew_per_second = '1000.5'",
                            "rate_hz = '0'"}) {
        EXPECT_TRUE(containsError(result, key)) << key;
    }
    EXPECT_EQ(result.errors.size(), 7U);   // "0.05 " is trimmed by the reader: fine
    EXPECT_EQ(result.config.statePort, kDefaultStatePort);
    EXPECT_EQ(result.config.ffbPort, kDefaultFfbPort);
    EXPECT_FLOAT_EQ(result.config.ffb.maxMagnitude, FfbSafetyConfig{}.maxMagnitude);   // the cap is never lifted
    EXPECT_FLOAT_EQ(result.config.telemetry.minChange, 0.05F);
}

// The limits themselves are allowed (the ranges are inclusive).
TEST(EmulatorConfig, AcceptsTheEndsOfEachRange) {
    const auto result = load("[emulator]\nstate_port = 1024\nffb_port = 65535\n[ffb]\nmax_magnitude = 0\n"
                             "max_slew_per_second = 1000\n");
    ASSERT_TRUE(result.errors.empty()) << result.errors.front();
    EXPECT_EQ(result.config.statePort, 1024);
    EXPECT_EQ(result.config.ffbPort, 65535);
    EXPECT_FLOAT_EQ(result.config.ffb.maxMagnitude, 0.0F);
}

// frozen_force_s: 0 (never) to 60 s; missing, the default 2 s; beyond the range or not a number, reported.
TEST(EmulatorConfig, ReadsHowLongAFrozenPushMayLast) {
    EXPECT_FLOAT_EQ(load("").config.ffb.frozenForceSeconds, 2.0F);
    for (const char* value : {"0", "60"}) {
        const auto result = load((std::string("[ffb]\nfrozen_force_s = ") + value + "\n").c_str());
        EXPECT_TRUE(result.errors.empty()) << value;
    }
    for (const char* value : {"-1", "61", "nan", "soon"}) {
        const auto result = load((std::string("[ffb]\nfrozen_force_s = ") + value + "\n").c_str());
        EXPECT_TRUE(containsError(result, "frozen_force_s")) << value;
        EXPECT_FLOAT_EQ(result.config.ffb.frozenForceSeconds, 2.0F) << value;
    }
}

// ignore_lone_full_push: true by default (GRID's broken value plays as none); false plays every full push.
TEST(EmulatorConfig, ReadsWhetherALoneFullPushIsIgnored) {
    EXPECT_TRUE(load("").config.ffb.ignoreLoneFullPush);
    EXPECT_FALSE(load("[ffb]\nignore_lone_full_push = false\n").config.ffb.ignoreLoneFullPush);
    const auto result = load("[ffb]\nignore_lone_full_push = sometimes\n");
    EXPECT_TRUE(containsError(result, "ignore_lone_full_push"));
    EXPECT_TRUE(result.config.ffb.ignoreLoneFullPush);
}

// [buttons] order: driver (the default) or native; anything else is reported and the default kept.
TEST(EmulatorConfig, ReadsTheButtonOrder) {
    EXPECT_EQ(load("").config.buttonOrder, ButtonOrder::Driver);
    EXPECT_EQ(load("[buttons]\norder = native\n").config.buttonOrder, ButtonOrder::Native);
    EXPECT_EQ(load("[buttons]\norder = driver ; as Logitech's driver\n").config.buttonOrder, ButtonOrder::Driver);
    const auto result = load("[buttons]\norder = hid\n");
    EXPECT_TRUE(containsError(result, "order")) << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_EQ(result.config.buttonOrder, ButtonOrder::Driver);
}

// Every spelling of a yes/no setting, and one that is neither (reported, the default kept).
TEST(EmulatorConfig, ReadsEverySpellingOfABool) {
    EXPECT_TRUE(load("[ffb]\ninvert = yes\n").config.ffbInvert);
    EXPECT_TRUE(load("[ffb]\ninvert = 1\n").config.ffbInvert);
    EXPECT_TRUE(load("[ffb]\ninvert = true\n").config.ffbInvert);
    EXPECT_FALSE(load("[ffb]\ngame_effects = no\n").config.gameFfb);
    EXPECT_FALSE(load("[ffb]\ngame_effects = 0\n").config.gameFfb);
    EXPECT_FALSE(load("[ffb]\ngame_effects = false\n").config.gameFfb);
    const auto bad = load("[ffb]\ninvert = True\ngame_effects = maybe\n");   // the words are lower case
    EXPECT_TRUE(containsError(bad, "[ffb] invert = 'True': expected true or false"));
    EXPECT_TRUE(containsError(bad, "[ffb] game_effects = 'maybe'"));
    EXPECT_FALSE(bad.config.ffbInvert);
    EXPECT_EQ(bad.config.gameFfb, EmulatorConfig{}.gameFfb);
}

// A source past the device's axes or buttons: reported, not read as some other control.
TEST(EmulatorConfig, RejectsSourcesPastTheLastControl) {
    const auto result = load("[throttle]\naxis = 8\n[buttons]\ntarget_1 = 64\n[hat]\nleft = 99\n");
    EXPECT_TRUE(containsError(result, "[throttle] axis = '8': expected -1, 0..7"));
    EXPECT_TRUE(containsError(result, "[buttons] target_1 = '64'"));
    EXPECT_TRUE(containsError(result, "[hat] left = '99'"));
    EXPECT_EQ(result.config.mapper.throttle.sourceAxis, MapperConfig{}.throttle.sourceAxis);
}

// A target button's secondary list naming a fourth accessory: refused whole (no half-read list), reported.
TEST(EmulatorConfig, SecondarySourcesOnTooManyAccessoriesAreRefused) {
    const auto result = load("[buttons]\ntarget_0 = 1:1/0\ntarget_1 = 1:2/0\ntarget_2 = 1:3/0\n"
                             "target_3_or = 5, 1:4/0\n");
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].find("[buttons] target_3_or = '5, 1:4/0': at most 3 accessories"), std::string::npos)
        << result.errors[0];
    EXPECT_FALSE(result.config.mapper.buttonSecondary[3][0].mapped());
}

// An axis name typed too long: cut to what the window shows, and reported.
TEST(EmulatorConfig, CutsTooLongAxisLabels) {
    const auto result = load("[axis_labels]\naxis_3 = Handbrake lever\n");
    EXPECT_TRUE(containsError(result, "[axis_labels] axis_3 = 'Handbrake lever': at most 12 characters"));
    EXPECT_EQ(result.config.mapper.sourceAxisLabels[3], "Handbrake le");
}

// [game_settings]: a key with no dot, an unknown game, an unknown setting, the steering-driven one, values out of
// range or not numbers. Each reported; good ones read.
TEST(EmulatorConfig, ReportsEveryKindOfBadGameSetting) {
    const auto result = load("[game_settings]\nnfsunbound = 0.5\nnfs1999.spring = 0.5\nnfsunbound.horn = 0.5\n"
                             "nfsunbound.rotation = 0.5\nnfsunbound.spring = 1.5\nnfsunbound.force_feedback = lots\n"
                             "nfsunbound.spring = -0.0001\nnfs1999 = 0.5\n");
    EXPECT_TRUE(containsError(result, "[game_settings] nfsunbound: NFS Unbound has no such setting"));
    EXPECT_TRUE(containsError(result, "[game_settings] nfs1999.spring: no built-in game profile nfs1999"));
    EXPECT_TRUE(containsError(result, "[game_settings] nfsunbound.horn: NFS Unbound has no such setting"));
    EXPECT_TRUE(containsError(result, "[game_settings] nfsunbound.rotation: follows the steering rotation"));
    EXPECT_TRUE(containsError(result, "nfsunbound.force_feedback = 'lots': expected a number from 0 to 1"));
    EXPECT_TRUE(containsError(result, "nfsunbound.spring = '-0.0001'"));   // the last of the two counts
    EXPECT_TRUE(containsError(result, "[game_settings] nfs1999: no built-in game profile nfs1999"));
    EXPECT_EQ(result.errors.size(), 7U);
    EXPECT_TRUE(result.config.gameSettings.empty());
    const auto good = load("[game_settings]\nnfsunbound.spring = 1\nnfsunbound.force_feedback = 0\n");
    ASSERT_TRUE(good.errors.empty()) << good.errors.front();
    EXPECT_FLOAT_EQ(good.config.gameSettings.at("nfsunbound").at("spring"), 1.0F);
    EXPECT_FLOAT_EQ(good.config.gameSettings.at("nfsunbound").at("force_feedback"), 0.0F);
}

// [game_ffb_gain]: an unknown game, a gain outside 0.25..4 or not a number. The ends are allowed.
TEST(EmulatorConfig, ReportsBadGameFfbGains) {
    const auto result = load("[game_ffb_gain]\nnfs1999 = 1\nnfs2015 = 4.5\nnfsunbound = strong\nnfspayback = 0.2\n");
    EXPECT_TRUE(containsError(result, "[game_ffb_gain] nfs1999: no built-in game profile"));
    EXPECT_TRUE(containsError(result, "[game_ffb_gain] nfs2015 = '4.5': expected a number from 0.25 to 4"));
    EXPECT_TRUE(containsError(result, "[game_ffb_gain] nfsunbound = 'strong'"));
    EXPECT_TRUE(containsError(result, "[game_ffb_gain] nfspayback = '0.2'"));
    EXPECT_TRUE(result.config.gameFfbGains.empty());
    const auto ends = load("[game_ffb_gain]\nnfs2015 = 4\nnfsunbound = 0.25\n");
    ASSERT_TRUE(ends.errors.empty()) << ends.errors.front();
    EXPECT_FLOAT_EQ(ends.config.gameFfbGains.at("nfs2015"), 4.0F);
    EXPECT_FLOAT_EQ(ends.config.gameFfbGains.at("nfsunbound"), 0.25F);
}

// [game_tuning]: a built-in game's values are read; an unknown game, a key without a game and bad values are reported
// and left out.
TEST(EmulatorConfig, ReadsGameTuning) {
    const auto result = load("[game_tuning]\nnfs2015.throttle_axis = slider\nnfsunbound.ffb_spring = 0.8\n"
                             "nfsunbound.ffb_cap = 2\nnfs1999.ffb_spring = 1\nffb_spring = 1\nnfspayback.colour = red\n");
    ASSERT_EQ(result.config.gameTunings.count("nfs2015"), 1U);
    EXPECT_EQ(result.config.gameTunings.at("nfs2015").pedalAxes[0], ObjectGuid::Slider);
    EXPECT_FLOAT_EQ(*result.config.gameTunings.at("nfsunbound").ffbGains[static_cast<std::size_t>(FfbGroup::Spring)],
                    0.8F);
    EXPECT_FALSE(result.config.gameTunings.at("nfsunbound").ffbCap.has_value());
    EXPECT_EQ(result.config.gameTunings.count("nfspayback"), 0U);   // nothing valid: no entry
    EXPECT_TRUE(containsError(result, "[game_tuning] nfsunbound.ffb_cap = '2': expected a number from 0 to 1"));
    EXPECT_TRUE(containsError(result, "[game_tuning] nfs1999.ffb_spring: no game nfs1999"));
    EXPECT_TRUE(containsError(result, "[game_tuning] ffb_spring: no game"));
    EXPECT_TRUE(containsError(result, "[game_tuning] nfspayback.colour = 'red': unknown key"));
    EXPECT_EQ(result.errors.size(), 4U);
}

// A game the user added is known to [game_tuning] only when it is passed in.
TEST(EmulatorConfig, GameTuningForAUserGame) {
    const auto parsed = IniDocument::parse("[game_tuning]\nexcalibur.ffb_damper = 0.5\n");
    ASSERT_TRUE(parsed.errors.empty());
    EXPECT_FALSE(loadEmulatorConfig(parsed.document).errors.empty());
    GameProfile excalibur;
    excalibur.id = "excalibur";
    excalibur.label = "Excalibur";
    excalibur.program = "excalibur.exe";
    const std::vector<GameProfile> users{excalibur};
    const auto result = loadEmulatorConfig(parsed.document, users);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_FLOAT_EQ(*result.config.gameTunings.at("excalibur").ffbGains[static_cast<std::size_t>(FfbGroup::Damper)],
                    0.5F);
}

// [game_rotation]: an unknown game and a rotation that is not one.
TEST(EmulatorConfig, ReportsBadGameRotations) {
    const auto result = load("[game_rotation]\nnfs1999 = 180\nnfs2015 = sideways\nnfsunbound = off\n");
    EXPECT_TRUE(containsError(result, "[game_rotation] nfs1999: no built-in game profile"));
    EXPECT_TRUE(containsError(result, "[game_rotation] nfs2015 = 'sideways': expected degrees (180) or off"));
    EXPECT_EQ(result.errors.size(), 2U);
    ASSERT_EQ(result.config.gameRotations.count("nfsunbound"), 1U);
    EXPECT_TRUE(result.config.gameRotations.at("nfsunbound").off);
}

// A bad flush level falls back to warn, like a bad level.
TEST(EmulatorConfig, InvalidFlushLevelFallsBackToWarn) {
    const auto result = load("[logging]\nflush_level = always\n");
    EXPECT_TRUE(containsError(result, "[logging] flush_level = 'always': expected a log level"));
    EXPECT_EQ(result.config.logging.flushLevel, "warn");
}

// Comments before the first header are no keys, and every section the reader knows is known (all empty here).
TEST(EmulatorConfig, KnownSectionsAndCommentsOnlyAreNoError) {
    const auto result = load("; only a comment\n[emulator]\n[steering]\n[throttle]\n[brake]\n[clutch]\n[buttons]\n"
                             "[button_labels]\n[axis_labels]\n[hat]\n[ffb]\n[logging]\n[telemetry]\n[retention]\n"
                             "[game_rotation]\n[game_settings]\n[game_ffb_gain]\n[game_tuning]\n");
    EXPECT_TRUE(result.errors.empty()) << result.errors.front();
}

// A config saved by Notepad (BOM, CRLF, comments after values, tabs): the same as the plain one.
TEST(EmulatorConfig, ReadsAWindowsSavedFileLikeAPlainOne) {
    const auto windows = load("\xEF\xBB\xBF[emulator]\r\ntarget_profile = g29\t; the old one\r\n[ffb]\r\n"
                              "\tmaster_gain\t=\t0.7 # quieter\r\ninvert = true\r\n");
    const auto plain = load("[emulator]\ntarget_profile = g29\n[ffb]\nmaster_gain = 0.7\ninvert = true\n");
    ASSERT_TRUE(windows.errors.empty()) << windows.errors.front();
    EXPECT_EQ(windows.config.targetProfile, plain.config.targetProfile);
    EXPECT_FLOAT_EQ(windows.config.ffb.masterGain, plain.config.ffb.masterGain);
    EXPECT_EQ(windows.config.ffbInvert, plain.config.ffbInvert);
}

// Integers past either end of their range are refused, and the default kept.
TEST(EmulatorConfig, IntegersOutsideTheirRange) {
    for (const char* text : {"[emulator]\nstate_rate_hz = 2001\n", "[emulator]\nstate_rate_hz = 9\n",
                             "[emulator]\nstate_rate_hz = 99999999999\n", "[emulator]\nstate_rate_hz = 50hz\n"}) {
        const auto result = load(text);
        EXPECT_TRUE(containsError(result, "state_rate_hz")) << text;
        EXPECT_EQ(result.config.stateRateHz, 500) << text;
    }
}

// [game_tuning] for a game the user added is read like a built-in game's; a key without a dot names no setting, even
// when the part before it is a game.
TEST(EmulatorConfig, GameTuningForUserGamesAndKeysWithoutADot) {
    GameProfile mine;
    mine.id = "excalibur";
    mine.label = "Excalibur";
    mine.program = "excalibur.exe";
    const std::vector<GameProfile> games{mine};
    const auto parsed = IniDocument::parse("[game_tuning]\nexcalibur.ffb_spring = 0.5\nexcalibur = 1\n");
    ASSERT_TRUE(parsed.errors.empty());
    const auto result = loadEmulatorConfig(parsed.document, games);
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].find("[game_tuning] excalibur: no game excalibur"), std::string::npos)
        << result.errors[0];
    ASSERT_EQ(result.config.gameTunings.count("excalibur"), 1U);
    EXPECT_FLOAT_EQ(*result.config.gameTunings.at("excalibur").ffbGains[static_cast<std::size_t>(FfbGroup::Spring)],
                    0.5F);
    EXPECT_TRUE(containsError(load("[game_tuning]\nexcalibur.ffb_spring = 0.5\n"), "no game excalibur"));
}

// [game_rotation] names a built-in profile or a game the user added (the Games tab sets it for either); any other name
// is a problem.
TEST(EmulatorConfig, GameRotationForAGameTheUserAdded) {
    const auto parsed = IniDocument::parse("[game_rotation]\nheat = 360\nnfspayback = off\nnone = 180\n");
    GameProfile heat;
    heat.id = "heat";
    heat.label = "NFS Heat";
    heat.program = "needforspeedheat.exe";
    const std::vector<GameProfile> userGames{heat};
    const EmulatorConfigResult result = loadEmulatorConfig(parsed.document, userGames);
    ASSERT_EQ(result.errors.size(), 1U);
    EXPECT_NE(result.errors[0].find("[game_rotation] none"), std::string::npos) << result.errors[0];
    ASSERT_EQ(result.config.gameRotations.count("heat"), 1U);
    EXPECT_FLOAT_EQ(result.config.gameRotations.at("heat").deg, 360.0F);
    EXPECT_TRUE(result.config.gameRotations.at("nfspayback").off);
    // Without the user's games, heat is no game.
    EXPECT_EQ(loadEmulatorConfig(parsed.document).errors.size(), 2U);
}

}  // namespace
}  // namespace wheelbri
