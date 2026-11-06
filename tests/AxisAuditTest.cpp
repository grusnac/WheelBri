// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/AxisAudit.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <thread>
#include <vector>

namespace wheelbri {
namespace {

// What NFS 2015 sent the G920 every frame (wheelbri-shim-2026-10-07.log): rpm 0, first light 0.01, redline 1.
constexpr std::array<std::uint8_t, 20> kNfs2015Lights{0x14, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0x0a, 0xd7, 0x23, 0x3c, 0x00, 0x00, 0x80, 0x3f};

TEST(AxisAudit, DecodesNfs2015RevLights) {
    const auto lights = decodeRevLights(0, kNfs2015Lights);
    ASSERT_TRUE(lights.has_value());
    EXPECT_FLOAT_EQ(lights->rpm, 0.0F);
    EXPECT_FLOAT_EQ(lights->firstLight, 0.01F);
    EXPECT_FLOAT_EQ(lights->redline, 1.0F);

    auto racing = kNfs2015Lights;   // "... 00 00 80 3f 0a d7 23 3c ...": rpm 1.0 in a race
    racing[10] = 0x80;
    racing[11] = 0x3f;
    EXPECT_FLOAT_EQ(decodeRevLights(0, racing)->rpm, 1.0F);
}

TEST(AxisAudit, OtherEscapesAreNotRevLights) {
    EXPECT_FALSE(decodeRevLights(1, kNfs2015Lights).has_value());                                // another command
    EXPECT_FALSE(decodeRevLights(0, std::span(kNfs2015Lights).first(16)).has_value());           // shorter
    auto version2 = kNfs2015Lights;
    version2[4] = 0x02;
    EXPECT_FALSE(decodeRevLights(0, version2).has_value());
    auto notANumber = kNfs2015Lights;   // rpm = NaN
    notANumber[8] = 0x00;
    notANumber[9] = 0x00;
    notANumber[10] = 0xc0;
    notANumber[11] = 0x7f;
    EXPECT_FALSE(decodeRevLights(0, notANumber).has_value());
}

TEST(AxisAudit, HexBytesShowsTheStartAndTheLength) {
    EXPECT_EQ(hexBytes(std::span(kNfs2015Lights).first(5)), "14 00 00 00 01");
    EXPECT_EQ(hexBytes(kNfs2015Lights, 3), "14 00 00 ... (20 bytes)");
    EXPECT_EQ(hexBytes({}), "");
}

TEST(AxisAudit, ScalingPropertiesCannotRemap) {
    for (const std::uint32_t id : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 9U}) {
        EXPECT_FALSE(setPropertyCouldRemap(id)) << id;
    }
    // FFLOAD, CALIBRATIONMODE, CALIBRATION, INSTANCENAME, PHYSICALRANGE, LOGICALRANGE, CPOINTS, APPDATA, unknown
    for (const std::uint32_t id : {8U, 10U, 11U, 13U, 18U, 19U, 21U, 22U, 99U}) {
        EXPECT_TRUE(setPropertyCouldRemap(id)) << id;
    }
}

TEST(AxisAudit, InputModules) {
    for (const char* name : {"LogitechSteeringWheelEnginesWrapper.dll", "LogiLed.dll", "HID.DLL", "xinput1_3.dll",
                             "XInput9_1_0.dll", "Windows.Gaming.Input.dll", "winusb.dll", "SDL2.dll", "dinput.dll",
                             "lghub_agent.dll"}) {
        EXPECT_TRUE(isInputModule(name)) << name;
    }
    for (const char* name : {"kernel32.dll", "dinput8.dll", "d3d11.dll", "NFS16.exe", "setupapi.dll"}) {
        EXPECT_FALSE(isInputModule(name)) << name;
    }
}

TEST(AxisAudit, CountsKindsInTheOrderFirstSeen) {
    AxisAudit audit;
    EXPECT_EQ(audit.record("EnumObjects", AxisAudit::Risk::None), 1U);
    EXPECT_EQ(audit.record("SetProperty DIPROP_RANGE", AxisAudit::Risk::None), 1U);
    EXPECT_EQ(audit.record("SetProperty DIPROP_RANGE", AxisAudit::Risk::None), 2U);
    EXPECT_EQ(audit.totalCalls(), 3U);
    EXPECT_EQ(audit.couldRemapCalls(), 0U);
    EXPECT_EQ(audit.summary(),
              "3 calls of 2 kinds; none could remap axes. Kinds: EnumObjects x1, SetProperty DIPROP_RANGE x2");
}

TEST(AxisAudit, NamesWhatCouldRemap) {
    AxisAudit audit;
    audit.record("Escape rev lights", AxisAudit::Risk::None);
    audit.record("SetActionMap", AxisAudit::Risk::CouldRemap);
    audit.record("SetActionMap", AxisAudit::Risk::CouldRemap);
    EXPECT_EQ(audit.couldRemapCalls(), 2U);
    EXPECT_EQ(audit.summary(),
              "3 calls of 2 kinds; COULD REMAP AXES: SetActionMap x2. Kinds: Escape rev lights x1, SetActionMap x2");
}

TEST(AxisAudit, EmptySummary) {
    EXPECT_EQ(AxisAudit{}.summary(), "0 calls of 0 kinds; none could remap axes");
}

TEST(AxisAudit, CountsFromSeveralThreads) {
    AxisAudit audit;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&audit] {
            for (int i = 0; i < 1000; ++i) audit.record("Escape rev lights", AxisAudit::Risk::None);
        });
    }
    for (std::thread& thread : threads) thread.join();
    EXPECT_EQ(audit.totalCalls(), 4000U);
}

// An Escape whose size field disagrees with the bytes passed, or whose first light or redline is not a number (a game
// writing garbage): not rev lights.
TEST(AxisAudit, RevLightsWithABadSizeFieldOrABadLightAreRefused) {
    auto wrongSize = kNfs2015Lights;
    wrongSize[0] = 0x18;
    EXPECT_FALSE(decodeRevLights(0, wrongSize).has_value());
    auto badFirst = kNfs2015Lights;   // first light = +infinity
    badFirst[12] = 0x00;
    badFirst[13] = 0x00;
    badFirst[14] = 0x80;
    badFirst[15] = 0x7f;
    EXPECT_FALSE(decodeRevLights(0, badFirst).has_value());
    auto badRedline = kNfs2015Lights;   // redline = NaN
    badRedline[18] = 0xc0;
    badRedline[19] = 0x7f;
    EXPECT_FALSE(decodeRevLights(0, badRedline).has_value());
}

// Every rule of the input-module list: Logitech's prefixes and names anywhere in the file name, SDL and XInput.
TEST(AxisAudit, InputModulesByEveryRule) {
    for (const char* name : {"LGSTrayHook.dll", "lg_ffb.dll", "Game_GHub_Bridge.dll", "MyLogitechPlugin.dll",
                             "SteeringWheelSDK.dll", "sdl3.dll", "xinputuap.dll", "WINUSB.DLL"}) {
        EXPECT_TRUE(isInputModule(name)) << name;
    }
    for (const char* name : {"", "lg.dll", "hid.dll.mui", "game_sdl.dll", "dinput.dll.bak"}) {
        EXPECT_FALSE(isInputModule(name)) << name;
    }
}

// Several kinds that could remap: all named, in the order first seen.
TEST(AxisAudit, NamesEveryKindThatCouldRemap) {
    AxisAudit audit;
    audit.record("SetActionMap", AxisAudit::Risk::CouldRemap);
    audit.record("EnumObjects", AxisAudit::Risk::None);
    audit.record("SetProperty DIPROP_APPDATA", AxisAudit::Risk::CouldRemap);
    EXPECT_EQ(audit.couldRemapCalls(), 2U);
    EXPECT_EQ(audit.summary(),
              "3 calls of 3 kinds; COULD REMAP AXES: SetActionMap x1, SetProperty DIPROP_APPDATA x1. Kinds: "
              "SetActionMap x1, EnumObjects x1, SetProperty DIPROP_APPDATA x1");
}

}  // namespace
}  // namespace wheelbri
