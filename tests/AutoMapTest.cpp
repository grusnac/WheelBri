#include "wheelbri/AutoMap.hpp"
#include "wheelbri/StartNeeds.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace wheelbri;

namespace {

constexpr DeviceId kWheelId{0x346E, 0x0005};
constexpr DeviceId kPedalsId{0x30B7, 0x1001};

// A rim with an Xbox-style face, a D-pad as four buttons, paddles and a Menu button; pedals on their own device.
Rig sampleRig() {
    Rig rig;
    RigDevice wheel;
    wheel.name = "Moza R3";
    wheel.id = kWheelId;
    wheel.kind = DeviceKind::Wheel;
    wheel.axes[0] = {"Wheel", "Steering"};
    wheel.buttons[0] = {"", "A"};
    wheel.buttons[1] = {"", "B"};
    wheel.buttons[2] = {"", "X"};
    wheel.buttons[3] = {"Cross", "Y"};
    wheel.buttons[4] = {"", "D Up"};
    wheel.buttons[5] = {"", "D Right"};
    wheel.buttons[6] = {"", "D Down"};
    wheel.buttons[7] = {"", "D Left"};
    wheel.buttons[12] = {"", "L Paddle"};
    wheel.buttons[13] = {"", "R Paddle"};
    wheel.buttons[35] = {"Start", "Menu"};   // not a core button: never maps itself
    wheel.buttons[20] = {"Horn", "Button"};
    RigDevice pedals;
    pedals.name = "Pedals";
    pedals.id = kPedalsId;
    pedals.kind = DeviceKind::Pedals;
    pedals.axes[1] = {"Gas", "Throttle"};
    pedals.axes[2] = {"", "Brake"};
    pedals.axes[3] = {"", "Clutch"};
    rig.devices = {wheel, pedals};
    return rig;
}

EmulatorConfig loadConfig(const std::string& text) {
    const auto parsed = IniDocument::parse(text);
    EXPECT_TRUE(parsed.errors.empty());
    auto result = loadEmulatorConfig(parsed.document);
    EXPECT_TRUE(result.errors.empty()) << (result.errors.empty() ? "" : result.errors.front());
    return result.config;
}

}  // namespace

TEST(AutoMap, G920TakesTheCoreInputsByWhatTheyAre) {
    const auto profile = makeProfile("g920");
    const AutoMapping found = autoMapping(sampleRig(), *profile, kWheelId);
    EXPECT_EQ(found.axes[MappingModel::kSteering], (SourceRef{{}, 0}));
    EXPECT_EQ(found.axes[MappingModel::kThrottle], (SourceRef{kPedalsId, 1}));
    EXPECT_EQ(found.axes[MappingModel::kBrake], (SourceRef{kPedalsId, 2}));
    EXPECT_EQ(found.axes[MappingModel::kClutch], (SourceRef{kPedalsId, 3}));
    EXPECT_EQ(found.hat, (std::array<SourceRef, 4>{SourceRef{{}, 4}, {{}, 5}, {{}, 6}, {{}, 7}}));
    EXPECT_EQ(found.buttons[0], (SourceRef{{}, 0}));    // A
    EXPECT_EQ(found.buttons[1], (SourceRef{{}, 1}));    // B
    EXPECT_EQ(found.buttons[2], (SourceRef{{}, 2}));    // X
    EXPECT_EQ(found.buttons[3], (SourceRef{{}, 3}));    // Y
    EXPECT_EQ(found.buttons[4], (SourceRef{{}, 13}));   // R Paddle
    EXPECT_EQ(found.buttons[5], (SourceRef{{}, 12}));   // L Paddle
    for (std::size_t t = 6; t < found.buttons.size(); ++t) EXPECT_FALSE(found.buttons[t].mapped()) << t;
    EXPECT_TRUE(found.notices.empty());
}

TEST(AutoMap, G923HasTheG920sCoreButtons) {
    const auto g920 = makeProfile("g920");
    const auto g923 = makeProfile("g923");
    for (std::size_t c = 0; c < kCoreButtonCount; ++c) {
        EXPECT_EQ(g923->coreButton(static_cast<CoreButton>(c)), g920->coreButton(static_cast<CoreButton>(c)));
    }
}

// The G25 and G27 have no A, B, X, Y: an Xbox-style rim's go on the black diamond by place.
TEST(AutoMap, G25AndG27PutTheFaceOnTheBlackDiamond) {
    for (const char* name : {"g25", "g27"}) {
        const auto profile = makeProfile(name);
        EXPECT_EQ(profile->buttonName(static_cast<std::size_t>(profile->coreButton(CoreButton::LPaddle))), "L Paddle");
        EXPECT_EQ(profile->buttonName(static_cast<std::size_t>(profile->coreButton(CoreButton::RPaddle))), "R Paddle");
        const AutoMapping found = autoMapping(sampleRig(), *profile, kWheelId);
        EXPECT_EQ(found.buttons[17], (SourceRef{{}, 0})) << name;   // A: bottom
        EXPECT_EQ(found.buttons[18], (SourceRef{{}, 1})) << name;   // B: right
        EXPECT_EQ(found.buttons[16], (SourceRef{{}, 2})) << name;   // X: left
        EXPECT_EQ(found.buttons[15], (SourceRef{{}, 3})) << name;   // Y: top
        EXPECT_EQ(found.buttons[4], (SourceRef{{}, 13})) << name;
        EXPECT_EQ(found.buttons[5], (SourceRef{{}, 12})) << name;
        EXPECT_FALSE(found.buttons[0].mapped()) << name;   // Red 1 is the user's to map
    }
    EXPECT_EQ(makeProfile("g25")->buttonName(17), "Blk bottom");
    EXPECT_EQ(makeProfile("g25")->buttonName(15), "Blk top");
}

TEST(AutoMap, AKindOnTwoControlsMapsNeither) {
    Rig rig = sampleRig();
    rig.devices[1].buttons[0] = {"", "A"};   // a button box's A too
    const AutoMapping found = autoMapping(rig, *makeProfile("g920"), kWheelId);
    EXPECT_FALSE(found.buttons[0].mapped());
    ASSERT_EQ(found.notices.size(), 1U);
    EXPECT_EQ(found.notices.front(), "2 controls in the Rig are A: none is used");
}

TEST(AutoMap, NothingWithoutAKind) {
    Rig rig = sampleRig();
    rig.devices[1].axes[3] = {"Clutch", ""};   // named, but nothing picked under "It is"
    const AutoMapping found = autoMapping(rig, *makeProfile("g920"), kWheelId);
    EXPECT_FALSE(found.axes[MappingModel::kClutch].mapped());
    EXPECT_TRUE(autoMapping(Rig{}, *makeProfile("g920"), {}).notices.empty());
}

TEST(AutoMap, TheWheelIsTheMappingsOwn) {
    EXPECT_EQ(mappingWheel("", sampleRig()), kWheelId);
    EXPECT_EQ(mappingWheel("30b7:1001", sampleRig()), kPedalsId);
    EXPECT_EQ(mappingWheel("Moza", sampleRig()), DeviceId{});
    // Another wheel: the rig's wheel is an accessory then.
    const AutoMapping found = autoMapping(sampleRig(), *makeProfile("g920"), kPedalsId);
    EXPECT_EQ(found.axes[MappingModel::kSteering], (SourceRef{kWheelId, 0}));
    EXPECT_EQ(found.axes[MappingModel::kThrottle], (SourceRef{{}, 1}));
}

TEST(AutoMap, ApplyRigFillsWhatTheMappingLeavesOut) {
    EmulatorConfig config = loadConfig("[emulator]\nfrom_rig = true\ntarget_profile = g920\n");
    EXPECT_TRUE(config.fromRig);
    config.sourceDeviceMatch = "346e:0005";   // as the session sets it from the rig's wheel
    EXPECT_TRUE(applyRig(config, sampleRig()).empty());
    const MapperConfig& mapper = config.mapper;
    EXPECT_EQ(mapper.steering.sourceAxis, 0);
    EXPECT_EQ(mapper.steering.sourceDevice, 0);
    EXPECT_EQ(mapper.throttle.sourceAxis, 1);
    EXPECT_EQ(mapper.throttle.sourceDevice, 1);
    EXPECT_EQ(mapper.accessories[0], kPedalsId);
    EXPECT_EQ(mapper.accessoryCount(), 1U);
    EXPECT_EQ(mapper.hatButtons.up, 4);
    EXPECT_EQ(mapper.hatButtons.left, 7);
    EXPECT_EQ(mapper.buttonMap[0], 0);
    EXPECT_EQ(mapper.buttonMap[4], 13);
    EXPECT_EQ(mapper.buttonMap[5], 12);
    for (std::size_t t = 6; t < mapper.buttonMap.size(); ++t) EXPECT_EQ(mapper.buttonMap[t], kUnmapped) << t;
}

// A line wins over the Rig, -1 included; Invert is the mapping's own.
TEST(AutoMap, ALineWins) {
    EmulatorConfig config = loadConfig(
        "[emulator]\nfrom_rig = true\n[throttle]\naxis = 30b7:1001/5\n[brake]\ninvert = true\n"
        "[buttons]\ntarget_0 = -1\ntarget_6 = 35\n[hat]\nup = -1\n");
    (void)applyRig(config, sampleRig());
    const MapperConfig& mapper = config.mapper;
    EXPECT_EQ(mapper.throttle.sourceAxis, 5);
    EXPECT_EQ(mapper.brake.sourceAxis, 2);
    EXPECT_TRUE(mapper.brake.invert);
    EXPECT_EQ(mapper.buttonMap[0], kUnmapped);
    EXPECT_EQ(mapper.buttonMap[1], 1);
    EXPECT_EQ(mapper.buttonMap[6], 35);
    EXPECT_EQ(mapper.hatButtons.up, kUnmapped);
    EXPECT_EQ(mapper.hatButtons.right, 5);
}

// A mapping written before from_rig: as it was (unlisted buttons 1:1), the Rig is not read.
TEST(AutoMap, AnOlderMappingIsAsItWas) {
    EmulatorConfig config = loadConfig("[throttle]\naxis = 2\n");
    EXPECT_FALSE(config.fromRig);
    const MapperConfig before = config.mapper;
    EXPECT_TRUE(applyRig(config, sampleRig()).empty());
    EXPECT_EQ(config.mapper.buttonMap, before.buttonMap);
    EXPECT_EQ(config.mapper.buttonMap[7], 7);
    EXPECT_EQ(config.mapper.steering.sourceAxis, 0);
    EXPECT_EQ(config.mapper.hatButtons.up, kUnmapped);
}

TEST(AutoMap, AFourthAccessoryIsLeftOut) {
    Rig rig = sampleRig();
    for (std::uint16_t pid = 1; pid <= 3; ++pid) {
        RigDevice box;
        box.id = {0x1234, pid};
        box.buttons[0] = {"", std::string(kAutoHatKinds[pid])};   // D Right, D Down, D Left on three more devices
        rig.devices.push_back(box);
        rig.devices[0].buttons[4 + pid] = {};
    }
    EmulatorConfig config = loadConfig("[emulator]\nfrom_rig = true\nsource_device = 346e:0005\n");
    const std::vector<std::string> notices = applyRig(config, rig);
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front(), "1234:0003/0: at most 3 accessories besides the wheel can be read at once");
    EXPECT_EQ(config.mapper.hatButtons.left, kUnmapped);
}

TEST(AutoMap, StartNeedsSeeTheRigsMapping) {
    EmulatorConfig config = loadConfig("[emulator]\nfrom_rig = true\n");
    const Rig rig = sampleRig();
    EXPECT_EQ(startNeeds(config, &rig).problem, kNoMapping);
    config.sourceDeviceMatch = "346e:0005";
    (void)applyRig(config, rig);
    EXPECT_EQ(startNeeds(config, &rig).problem, "");
}

// The Mapping tab: the Rig's sources shown, written with no line; a picked one is the user's.
TEST(AutoMap, TheMappingTabWritesOnlyWhatTheUserPicked) {
    const std::string base = "[emulator]\nfrom_rig = true\ntarget_profile = g920\n[brake]\ninvert = true\n";
    MappingModel model = mappingModel(loadConfig(base));
    const auto profile = makeProfile("g920");
    EXPECT_TRUE(followRig(model, sampleRig(), *profile).empty());
    EXPECT_EQ(model.buttons[0], (SourceRef{{}, 0}));
    EXPECT_EQ(model.axes[MappingModel::kThrottle], (SourceRef{kPedalsId, 1}));
    EXPECT_TRUE(controlsUsedTwice(model, *profile).empty());

    model.buttons[6] = {{}, 35};   // Menu <- Start, picked
    model.autoInputs.buttons[6] = false;
    model.hat[0] = {};             // D-pad Up cleared
    model.autoInputs.hat[0] = false;
    const MappingWriteResult written = writeMapping(base, model);
    ASSERT_TRUE(written.error.empty()) << written.error;
    const IniDocument ini = IniDocument::parse(written.text).document;
    EXPECT_EQ(ini.get("emulator", "from_rig"), "true");
    EXPECT_FALSE(ini.get("steering", "axis"));
    EXPECT_FALSE(ini.get("throttle", "axis"));
    EXPECT_EQ(ini.get("brake", "invert"), "true");
    EXPECT_FALSE(ini.get("buttons", "target_0"));
    EXPECT_FALSE(ini.get("buttons", "target_11"));
    EXPECT_EQ(ini.get("buttons", "target_6"), "35");
    EXPECT_EQ(ini.get("hat", "up"), "-1");
    EXPECT_FALSE(ini.get("hat", "right"));

    // Read back: the same.
    MappingModel back = mappingModel(loadConfig(written.text));
    (void)followRig(back, sampleRig(), *profile);
    EXPECT_EQ(back.buttons, model.buttons);
    EXPECT_EQ(back.hat, model.hat);
    EXPECT_EQ(back.axes, model.axes);
    EXPECT_EQ(back.autoInputs, model.autoInputs);
}
