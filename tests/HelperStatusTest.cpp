// SPDX-License-Identifier: GPL-3.0-or-later
// HelperStatus: what a Hello's outcome means for the Logs tab's Helper row, and its text.

#include "wheelbri/HelperStatus.hpp"

#include <gtest/gtest.h>

namespace wheelbri::helper {
namespace {

constexpr const char* kImage = R"(C:\Program Files\WheelBri\wheelbri-helper.exe)";

TEST(HelperStatus, VersionFromTheHelloText) {
    EXPECT_EQ(helperVersionOf("WheelBri Helper 0.9.0"), "0.9.0");
    EXPECT_EQ(helperVersionOf("WheelBri Helper 0.9.0: only WheelBri (...) may ask"), "0.9.0");
    EXPECT_EQ(helperVersionOf("WheelBri Helper "), "");
    EXPECT_EQ(helperVersionOf("WheelBri Helper : why"), "");
    EXPECT_EQ(helperVersionOf("WheelBri Helper 0.9 beta"), "");   // not one word
    EXPECT_EQ(helperVersionOf("Somebody Else 1.0"), "");
    EXPECT_EQ(helperVersionOf(""), "");
}

TEST(HelperStatus, AnAcceptedHelloIsRunningForThisApp) {
    const HelperState state = helperStateFromHello(Reply{1, Status::Ok, "WheelBri Helper 0.9.0"}, {}, kImage);
    EXPECT_EQ(state.kind, HelperState::Kind::Accepted);
    EXPECT_EQ(state.version, "0.9.0");
    EXPECT_EQ(state.detail, kImage);
    EXPECT_EQ(describeHelper(state), "running 0.9.0, serves this app");
}

TEST(HelperStatus, ARefusedHelloSaysWhyWithoutTheVersionAgain) {
    const HelperState state = helperStateFromHello(
        Reply{1, Status::Refused, "WheelBri Helper 0.9.0: only WheelBri in C:\\Program Files\\WheelBri may ask"}, {},
        kImage);
    EXPECT_EQ(state.kind, HelperState::Kind::Refused);
    EXPECT_EQ(state.version, "0.9.0");
    EXPECT_EQ(state.detail, "only WheelBri in C:\\Program Files\\WheelBri may ask");
    EXPECT_EQ(describeHelper(state), "running 0.9.0, not for this copy of WheelBri");
}

TEST(HelperStatus, ARefusalNotInTheHelpersWordsIsKeptWhole) {
    HelperState state = helperStateFromHello(Reply{1, Status::Refused, "unreadable message: x: y"}, {}, {});
    EXPECT_EQ(state.kind, HelperState::Kind::Refused);
    EXPECT_EQ(state.version, "");
    EXPECT_EQ(state.detail, "unreadable message: x: y");
    state = helperStateFromHello(Reply{1, Status::Refused, "WheelBri Helper 0.9.0"}, {}, {});   // a version, no why
    EXPECT_EQ(state.detail, "WheelBri Helper 0.9.0");
    state.version.clear();
    EXPECT_EQ(describeHelper(state), "running, not for this copy of WheelBri");
}

TEST(HelperStatus, NoAnswerIsAFailureWithTheReason) {
    HelperState state = helperStateFromHello(std::nullopt, "the helper did not answer within 5 s", {});
    EXPECT_EQ(state.kind, HelperState::Kind::Failed);
    EXPECT_EQ(state.detail, "the helper did not answer within 5 s");
    EXPECT_EQ(describeHelper(state), "cannot ask it: the helper did not answer within 5 s");
    state = helperStateFromHello(std::nullopt, {}, {});
    EXPECT_EQ(state.detail, "no answer");
}

TEST(HelperStatus, AnyOtherAnswerToHelloIsAFailure) {
    for (const Status status : {Status::Failed, Status::Pending, Status::Unsupported}) {
        const HelperState state = helperStateFromHello(Reply{1, status, "odd"}, {}, kImage);
        EXPECT_EQ(state.kind, HelperState::Kind::Failed);
        EXPECT_EQ(state.detail, std::string(statusName(status)) + ": odd");
    }
    const HelperState empty = helperStateFromHello(Reply{1, Status::Failed, {}}, {}, kImage);
    EXPECT_EQ(empty.detail, "failed");
}

TEST(HelperStatus, EveryKindHasItsText) {
    HelperState state;
    EXPECT_EQ(describeHelper(state), "checking...");
    state.kind = HelperState::Kind::NotWindows;
    EXPECT_EQ(describeHelper(state), "Windows only");
    state.kind = HelperState::Kind::NotRunning;
    EXPECT_EQ(describeHelper(state), "not running (not installed, or stopped)");
    state.kind = HelperState::Kind::Accepted;
    EXPECT_EQ(describeHelper(state), "running, serves this app");   // no version known
    state.kind = HelperState::Kind::Failed;
    EXPECT_EQ(describeHelper(state), "cannot ask it");
    state.kind = static_cast<HelperState::Kind>(99);
    EXPECT_EQ(describeHelper(state), "unknown");
}

}  // namespace
}  // namespace wheelbri::helper
