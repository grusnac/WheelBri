#include "wheelbri/LogiSdk.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace wheelbri::logi;

TEST(LogiSdk, TheFortyEightNamesPaybackLooksUp) {
    // From NFS Payback's program (2026-10-10), in its order: the DLL must export every one of them.
    static constexpr const char* kPayback[] = {
        "LogiSteeringInitializeWithWindow", "LogiSteeringInitialize", "LogiSteeringShutdown", "LogiUpdate",
        "LogiGetStateENGINES", "LogiGetState", "LogiGetDevicePath", "LogiGetFriendlyProductName", "LogiIsConnected",
        "LogiIsDeviceConnected", "LogiIsManufacturerConnected", "LogiIsModelConnected", "LogiButtonTriggered",
        "LogiButtonReleased", "LogiButtonIsPressed", "LogiGenerateNonLinearValues", "LogiGetNonLinearValue",
        "LogiHasForceFeedback", "LogiIsPlaying", "LogiPlaySpringForce", "LogiStopSpringForce",
        "LogiPlayConstantForce", "LogiStopConstantForce", "LogiPlayDamperForce", "LogiStopDamperForce",
        "LogiPlaySideCollisionForce", "LogiPlayFrontalCollisionForce", "LogiPlayDirtRoadEffect",
        "LogiStopDirtRoadEffect", "LogiPlayBumpyRoadEffect", "LogiStopBumpyRoadEffect", "LogiPlaySlipperyRoadEffect",
        "LogiStopSlipperyRoadEffect", "LogiPlaySurfaceEffect", "LogiStopSurfaceEffect", "LogiPlayCarAirborne",
        "LogiStopCarAirborne", "LogiPlaySoftstopForce", "LogiStopSoftstopForce",
        "LogiSetPreferredControllerProperties", "LogiGetCurrentControllerProperties", "LogiGetShifterMode",
        "LogiSetOperatingRange", "LogiGetOperatingRange", "LogiPlayLeds", "LogiSetOperatingRangeDInput",
        "LogiGetOperatingRangeDInput", "LogiPlayLedsDInput"};
    ASSERT_EQ(functions().size(), std::size(kPayback));
    for (std::size_t i = 0; i < std::size(kPayback); ++i) {
        EXPECT_EQ(functions()[i].name, kPayback[i]);
        EXPECT_EQ(functionIndex(kPayback[i]), i);
    }
    EXPECT_EQ(functionIndex("LogiGetDiState"), functions().size());   // a G HUB 9.x extra Payback never asks for
    EXPECT_EQ(findFunction("nope"), nullptr);
}

TEST(LogiSdk, EveryFunctionFitsInFourRegisters) {
    std::set<std::string_view> names;
    for (const Function& f : functions()) {
        EXPECT_LE(f.args, 4U) << f.name;
        EXPECT_LE(f.keyArgs, f.args) << f.name;
        EXPECT_TRUE(names.insert(f.name).second) << f.name;
        for (std::size_t i = 0; i < f.args; ++i) EXPECT_FALSE(f.argNames[i].empty()) << f.name << " arg " << i;
    }
}

TEST(LogiSdk, FormatsArgumentsByKind) {
    const std::array<std::int64_t, 4> spring{0, 0, 41, 90};
    EXPECT_EQ(formatCall(*findFunction("LogiPlaySpringForce"), spring),
              "LogiPlaySpringForce(index 0, offset 0%, saturation 41%, coefficient 90%)");
    // A 32-bit int arrives with whatever the caller left in the register's upper half.
    const std::array<std::int64_t, 4> model{0x1234567800000000LL, 0x0BCD00000000001BLL, 0, 0};
    EXPECT_EQ(formatCall(*findFunction("LogiIsModelConnected"), model), "LogiIsModelConnected(index 0, model G920)");
    const std::array<std::int64_t, 4> surface{0, 1, -30, 75};
    EXPECT_EQ(formatCall(*findFunction("LogiPlaySurfaceEffect"), surface),
              "LogiPlaySurfaceEffect(index 0, type square, magnitude -30%, period 75 ms)");
    const std::array<std::int64_t, 4> leds{0, std::bit_cast<std::uint32_t>(0.25F), std::bit_cast<std::uint32_t>(0.01F),
                                           std::bit_cast<std::uint32_t>(1.0F)};
    EXPECT_EQ(formatCall(*findFunction("LogiPlayLeds"), leds),
              "LogiPlayLeds(index 0, rpm 0.250, first led 0.010, redline 1.000)");
    const std::array<std::int64_t, 4> none{};
    EXPECT_EQ(formatCall(*findFunction("LogiUpdate"), none), "LogiUpdate()");
    const std::array<std::int64_t, 4> init{0x100, 0x40be8, 0, 0};   // bool false in the low byte, garbage above
    EXPECT_EQ(formatCall(*findFunction("LogiSteeringInitializeWithWindow"), init),
              "LogiSteeringInitializeWithWindow(ignore XInput false, window 0x40be8)");
}

TEST(LogiSdk, FormatsResults) {
    EXPECT_EQ(formatResult(*findFunction("LogiIsConnected"), 0x7fff0001), "true");
    EXPECT_EQ(formatResult(*findFunction("LogiIsConnected"), 0x7fff0100), "false");
    EXPECT_EQ(formatResult(*findFunction("LogiGetShifterMode"), 0xFFFFFFFF), "-1");
    EXPECT_EQ(formatResult(*findFunction("LogiSteeringShutdown"), 42), "void");
    EXPECT_EQ(formatResult(*findFunction("LogiGetState"), 0), "null");
}

TEST(LogiSdk, NamesConstants) {
    EXPECT_EQ(modelName(0), "G27");
    EXPECT_EQ(modelName(26), "G29");
    EXPECT_EQ(modelName(27), "G920");
    EXPECT_EQ(modelName(28), "unknown 28");
    EXPECT_EQ(manufacturerName(0), "Logitech");
    EXPECT_EQ(manufacturerName(-1), "none");
    EXPECT_EQ(deviceTypeName(0), "wheel");
    EXPECT_EQ(forceTypeName(1), "constant");
    EXPECT_EQ(forceTypeName(9), "unknown 9");   // a gap in the SDK's numbering
    EXPECT_EQ(forceTypeName(11), "car airborne");
    EXPECT_EQ(periodicTypeName(2), "triangle");
}

TEST(LogiSdk, DecodesControllerProperties) {
    std::array<std::uint8_t, kPropertiesSize> bytes{};
    const auto put = [&](std::size_t offset, std::int32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    bytes[0] = 1;
    put(4, 100);
    put(8, 80);
    put(12, 60);
    bytes[16] = 0;
    put(20, 30);
    bytes[24] = 1;
    put(28, 360);
    bytes[32] = 1;
    bytes[33] = 0;
    const auto p = decodeProperties(bytes);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(describe(*p), "force on, overall gain 100%, spring gain 80%, damper gain 60%, default spring off at 30%, "
                            "combined pedals yes, wheel range 360 deg, game settings on (allowed no)");
    EXPECT_FALSE(decodeProperties(std::span(bytes).first(35)).has_value());
}

TEST(LogiSdk, DecodesTheStatePrefix) {
    std::vector<std::uint8_t> bytes(272, 0);   // a whole DIJOYSTATE2
    const auto put = [&](std::size_t offset, std::int32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put(0, 18551);
    put(4, -32768);
    put(20, 32767);
    put(24, 32767);
    put(32, -1);
    bytes[48 + 0] = 0x80;
    bytes[48 + 5] = 0x80;
    const auto s = decodeState(bytes);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->x, 18551);
    EXPECT_EQ(s->buttons, 0x21U);
    EXPECT_EQ(describe(*s), "x=18551 y=-32768 z=0 rx=0 ry=0 rz=32767 s0=32767 s1=0 pov0=-1 buttons=00000021");
    EXPECT_FALSE(decodeState(std::span(bytes).first(79)).has_value());
}

TEST(LogiSdk, ChangeLogSaysFirstChangedSame) {
    ChangeLog log;
    EXPECT_EQ(log.observe(3, 0, 0, "a"), ChangeLog::Seen::First);
    EXPECT_EQ(log.observe(3, 0, 0, "a"), ChangeLog::Seen::Same);
    EXPECT_EQ(log.observe(3, 0, 0, "b"), ChangeLog::Seen::Changed);
    EXPECT_EQ(log.observe(3, 0, 1, "b"), ChangeLog::Seen::First);   // another key (button 1)
    EXPECT_EQ(log.observe(4, 0, 0, "b"), ChangeLog::Seen::First);   // another function
    EXPECT_EQ(log.observe(3, 0, 0, "b"), ChangeLog::Seen::Same);
}

TEST(LogiSdk, NewestVersionFolder) {
    const std::vector<std::string> names{"9_1_1", "9_0_10", "10_0_2", "readme", "9__1", "", "9_1_1a"};
    EXPECT_EQ(newestVersionFolder(names), "10_0_2");
    const std::vector<std::string> ghub{"9_0_10", "9_1_1"};
    EXPECT_EQ(newestVersionFolder(ghub), "9_1_1");
    const std::vector<std::string> none{"logs", ""};
    EXPECT_EQ(newestVersionFolder(none), "");
}
