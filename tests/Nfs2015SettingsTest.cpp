#include "wheelbri/Nfs2015Settings.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wheelbri {
namespace {

// The lines that matter, as NFS 2015 wrote them (2026-10-09, custom wheel controls), with CRLF like the real file.
std::string profile(const int custom, const int accelerate, const int brake, const int wheelType = 3) {
    const auto binding = [&](const std::string& action, const int pad, const int key, const int wheel) {
        const std::string prefix = "GstKeyBinding.racevehicle." + action + ".";
        return prefix + "0.axis 24\r\n" + prefix + "0.button " + std::to_string(pad) + "\r\n" + prefix +
               "0.negate 0\r\n" + prefix + "0.type 2\r\n" + prefix + "1.axis 0\r\n" + prefix + "1.button " +
               std::to_string(key) + "\r\n" + prefix + "1.negate 0\r\n" + prefix + "1.type 0\r\n" + prefix +
               "2.axis 24\r\n" + prefix + "2.button " + std::to_string(wheel) + "\r\n" + prefix +
               "2.negate 0\r\n" + prefix + "2.type " + std::to_string(wheelType) + "\r\n";
    };
    return "GstInput.AutoReverseForManualGears 1\r\nGstInput.CustomControlsPad 0\r\nGstInput.CustomControlsWheel " +
           std::to_string(custom) + "\r\n" + binding("ConceptBrake", 14, 208, brake) +
           binding("ConceptHandBrake", 6, 57, 6) + binding("ConceptMoveFB", 15, 200, accelerate);
}

using Verdict = Nfs2015WheelBindings::Verdict;

TEST(Nfs2015Settings, DefaultWheelControls) {
    EXPECT_EQ(checkNfs2015Settings(profile(0, 25, 24)).verdict, Verdict::Defaults);   // its bindings are not used
}

TEST(Nfs2015Settings, CustomAsBoundWithWheelBri) {
    const Nfs2015WheelBindings bindings = checkNfs2015Settings(profile(1, 24, 25));
    EXPECT_EQ(bindings.verdict, Verdict::AsExpected);
    EXPECT_EQ(bindings.accelerate, 24);
    EXPECT_EQ(bindings.brake, 25);
}

TEST(Nfs2015Settings, TheOldWorkaroundIsFound) {
    // Accelerate on the brake input and brake on the clutch: what a player binds to fix the G920 without WheelBri.
    const Nfs2015WheelBindings bindings = checkNfs2015Settings(profile(1, 25, 26));
    EXPECT_EQ(bindings.verdict, Verdict::PedalsMoved);
    EXPECT_EQ(bindings.accelerate, 25);
    EXPECT_EQ(bindings.brake, 26);
    EXPECT_EQ(checkNfs2015Settings(profile(1, 24, 26)).verdict, Verdict::PedalsMoved);   // only one of them
}

TEST(Nfs2015Settings, BoundToNoWheelInput) {
    const Nfs2015WheelBindings bindings = checkNfs2015Settings(profile(1, 24, 25, 2));
    EXPECT_EQ(bindings.verdict, Verdict::PedalsMoved);
    EXPECT_EQ(bindings.accelerate, -1);
}

TEST(Nfs2015Settings, WheelBindingInAnotherSlot) {
    std::string text = profile(1, 24, 25);
    // Swap slot 2 and 0's types for brake: the wheel binding is found by its type, not its slot.
    const std::string from = "ConceptBrake.2.type 3";
    text.replace(text.find(from), from.size(), "ConceptBrake.2.type 2");
    const std::string pad = "ConceptBrake.0.type 2";
    text.replace(text.find(pad), pad.size(), "ConceptBrake.0.type 3");
    const Nfs2015WheelBindings bindings = checkNfs2015Settings(text);
    EXPECT_EQ(bindings.brake, 14);   // slot 0's button
    EXPECT_EQ(bindings.verdict, Verdict::PedalsMoved);
}

TEST(Nfs2015Settings, AnythingElseIsUnreadable) {
    EXPECT_EQ(checkNfs2015Settings("").verdict, Verdict::Unreadable);
    EXPECT_EQ(checkNfs2015Settings("not a profile\nat all").verdict, Verdict::Unreadable);
    EXPECT_EQ(checkNfs2015Settings("GstInput.CustomControlsWheel x\r\n").verdict, Verdict::Unreadable);
    EXPECT_EQ(checkNfs2015Settings("GstInput.CustomControlsWheel 1\r\n").verdict, Verdict::Unreadable);
    // A longer key that starts the same is another key.
    EXPECT_EQ(checkNfs2015Settings("GstInput.CustomControlsWheelX 0\n").verdict, Verdict::Unreadable);
    std::string noButton = profile(1, 24, 25);
    const std::string line = "GstKeyBinding.racevehicle.ConceptMoveFB.2.button 24\r\n";
    noButton.erase(noButton.find(line), line.size());
    EXPECT_EQ(checkNfs2015Settings(noButton).verdict, Verdict::Unreadable);
}

TEST(Nfs2015Settings, LfOnlyAndNoFinalNewline) {
    std::string text = profile(1, 24, 25);
    std::string lf;
    for (const char c : text) {
        if (c != '\r') lf.push_back(c);
    }
    lf.pop_back();
    EXPECT_EQ(checkNfs2015Settings(lf).verdict, Verdict::AsExpected);
}

TEST(Nfs2015Settings, InputNames) {
    EXPECT_EQ(nfs2015WheelInputName(24), "Throttle");
    EXPECT_EQ(nfs2015WheelInputName(25), "Brake");
    EXPECT_EQ(nfs2015WheelInputName(26), "Clutch");
    EXPECT_EQ(nfs2015WheelInputName(14), "Ltrigger");
    EXPECT_EQ(nfs2015WheelInputName(38), "Undefined");
    EXPECT_EQ(nfs2015WheelInputName(39), "input 39");
    EXPECT_EQ(nfs2015WheelInputName(-1), "not a wheel input");
}

}  // namespace
}  // namespace wheelbri
