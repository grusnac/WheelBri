// The WheelBri Helper's session (what one connection asked for, and its undoing), Windows only: HelperSession with
// fakes for the privileged steps (HelperSession::Steps), so nothing is written to HKLM, no script runs and no device
// changes. The caller's process is this test program's own; nothing here needs Administrator.

#include "HelperSession.hpp"

#include "wheelbri/HelperProtocol.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace wheelbri::helper {
namespace {

using service::privileged::HideSetup;

// What the fake steps were asked, and what they answer.
struct Fake {
    std::uint32_t allowedPid = 0;   // HKLM's AllowedPid as the fakes keep it
    bool allowOnlyWorks = true;
    bool writeWorks = true;
    std::string problem;            // interfaceProblem's answer for every id
    HideSetup putAnswer = HideSetup::Running;
    HideSetup takeAnswer = HideSetup::Running;
    bool installWorks = true;
    std::vector<std::vector<std::string>> putCalls;
    std::vector<std::vector<std::string>> takeCalls;
    std::vector<std::filesystem::path> scriptLogFolders;
    std::vector<LogLevel> levels;
    int installs = 0;
};

HelperSession::Steps fakeSteps(Fake& fake) {
    HelperSession::Steps steps;
    steps.allowOnly = [&fake](void* const process, std::string& message) {
        if (!fake.allowOnlyWorks) {
            message = "cannot write AllowedPid";
            return false;
        }
        fake.allowedPid = GetProcessId(static_cast<HANDLE>(process));
        return true;
    };
    steps.allowedPid = [&fake] { return fake.allowedPid; };
    steps.writeAllowedPid = [&fake](const std::uint32_t pid, std::uint64_t) {
        if (!fake.writeWorks) return false;
        fake.allowedPid = pid;
        return true;
    };
    steps.interfaceProblem = [&fake](const std::string&) { return fake.problem; };
    steps.putHideFilter = [&fake](const std::vector<std::string>& install, const std::vector<std::string>&, int,
                                  const std::filesystem::path& logFolder, const HelperSession::Cancelled&,
                                  std::string& message) {
        fake.putCalls.push_back(install);
        fake.scriptLogFolders.push_back(logFolder);
        if (fake.putAnswer != HideSetup::Running) message = "put: not yet";
        return fake.putAnswer;
    };
    steps.takeHideFilterOff = [&fake](const std::vector<std::string>& interfaces, const HelperSession::Cancelled&,
                                      std::string& message) {
        fake.takeCalls.push_back(interfaces);
        if (fake.takeAnswer != HideSetup::Running) message = "take: not yet";
        return fake.takeAnswer;
    };
    steps.installWheelDriver = [&fake](int, const std::filesystem::path& logFolder, const HelperSession::Cancelled&,
                                       std::string& message) {
        ++fake.installs;
        fake.scriptLogFolders.push_back(logFolder);
        if (!fake.installWorks) message = "pnputil failed";
        return fake.installWorks;
    };
    steps.writeLogLevel = [&fake](const LogLevel level, std::string&) {
        fake.levels.push_back(level);
        return true;
    };
    return steps;
}

// A handle to this process the session may keep and close, as the server gives it the caller's.
void* ownProcess() {
    HANDLE process = nullptr;
    EXPECT_TRUE(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
                                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL, FALSE, 0));
    return process;
}

std::unique_ptr<HelperSession> session(Fake& fake) {
    return std::make_unique<HelperSession>(ownProcess(), nullptr, 0, [] { return false; }, fakeSteps(fake));
}

Request request(const RequestType type, std::vector<std::string> devices = {}, const std::uint32_t seq = 7) {
    Request r;
    r.type = type;
    r.seq = seq;
    r.devices = std::move(devices);
    return r;
}

const std::uint32_t kOwnPid = GetCurrentProcessId();

// Hello belongs to the server (the version), not to a session.
TEST(HelperSession, HelloIsNotASessionRequest) {
    Fake fake;
    const Reply reply = session(fake)->handle(request(RequestType::Hello, {}, 3));
    EXPECT_EQ(reply.seq, 3U);
    EXPECT_EQ(reply.status, Status::Unsupported);
}

// HideRig: only the caller may open the rig; the session's end shows it again.
TEST(HelperSession, TheRigHiddenIsShownAgainWhenTheSessionEnds) {
    Fake fake;
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::HideRig)).status, Status::Ok);
    EXPECT_EQ(fake.allowedPid, kOwnPid);
    s.reset();
    EXPECT_EQ(fake.allowedPid, 0U);
}

// ShowRig undoes HideRig at once; without a HideRig it changes nothing.
TEST(HelperSession, ShowRigUndoesOnlyItsOwnHide) {
    Fake fake;
    fake.allowedPid = 1234;   // another app session's
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::ShowRig)).status, Status::Ok);
    EXPECT_EQ(fake.allowedPid, 1234U);
    EXPECT_EQ(s->handle(request(RequestType::HideRig)).status, Status::Ok);
    EXPECT_EQ(s->handle(request(RequestType::ShowRig)).status, Status::Ok);
    EXPECT_EQ(fake.allowedPid, 0U);
}

// Someone hid the rig after this session did: the session's end leaves theirs alone.
TEST(HelperSession, AnotherSessionsHideIsLeftAlone) {
    Fake fake;
    auto s = session(fake);
    ASSERT_EQ(s->handle(request(RequestType::HideRig)).status, Status::Ok);
    fake.allowedPid = 4321;
    s.reset();
    EXPECT_EQ(fake.allowedPid, 4321U);
}

// AllowedPid could not be written: Failed, and nothing to undo.
TEST(HelperSession, AHideThatFailsIsNotUndone) {
    Fake fake;
    fake.allowOnlyWorks = false;
    fake.allowedPid = 99;
    auto s = session(fake);
    const Reply reply = s->handle(request(RequestType::HideRig));
    EXPECT_EQ(reply.status, Status::Failed);
    EXPECT_EQ(reply.text, "cannot write AllowedPid");
    s.reset();
    EXPECT_EQ(fake.allowedPid, 99U);
}

// ShowRig that cannot write HKLM says so, and the session's end tries again.
TEST(HelperSession, ShowRigThatCannotWriteFailsAndIsTriedAgain) {
    Fake fake;
    auto s = session(fake);
    ASSERT_EQ(s->handle(request(RequestType::HideRig)).status, Status::Ok);
    fake.writeWorks = false;
    EXPECT_EQ(s->handle(request(RequestType::ShowRig)).status, Status::Failed);
    EXPECT_EQ(fake.allowedPid, kOwnPid);
    fake.writeWorks = true;
    s.reset();
    EXPECT_EQ(fake.allowedPid, 0U);
}

// An interface the helper must not touch (a keyboard, one not connected): refused before any script runs.
TEST(HelperSession, PutHideFilterRefusesABadInterface) {
    Fake fake;
    fake.problem = "HID\\X is a keyboard: never hidden";
    auto s = session(fake);
    const Reply reply = s->handle(request(RequestType::PutHideFilter, {"HID\\X"}));
    EXPECT_EQ(reply.status, Status::Refused);
    EXPECT_EQ(reply.text, fake.problem);
    EXPECT_TRUE(fake.putCalls.empty());
    s.reset();
    EXPECT_TRUE(fake.takeCalls.empty());
}

// The filter put on (running or waiting for a restart) comes off when the session ends; each interface once.
TEST(HelperSession, TheFilterPutOnComesOffWhenTheSessionEnds) {
    Fake fake;
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::PutHideFilter, {"HID\\A", "HID\\B"})).status, Status::Ok);
    fake.putAnswer = HideSetup::Waiting;
    const Reply waiting = s->handle(request(RequestType::PutHideFilter, {"HID\\B", "HID\\C"}));
    EXPECT_EQ(waiting.status, Status::Pending);
    EXPECT_EQ(waiting.text, "put: not yet");
    ASSERT_EQ(fake.putCalls.size(), 2U);
    s.reset();
    ASSERT_EQ(fake.takeCalls.size(), 1U);
    EXPECT_EQ(fake.takeCalls[0], (std::vector<std::string>{"HID\\A", "HID\\B", "HID\\C"}));
}

// A put that failed or had nothing to do leaves nothing to take off.
TEST(HelperSession, AFailedPutLeavesNothingToTakeOff) {
    Fake fake;
    fake.putAnswer = HideSetup::Failed;
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::PutHideFilter, {"HID\\A"})).status, Status::Failed);
    fake.putAnswer = HideSetup::NothingToDo;
    EXPECT_EQ(s->handle(request(RequestType::PutHideFilter, {"HID\\B"})).status, Status::Ok);
    s.reset();
    EXPECT_TRUE(fake.takeCalls.empty());
}

// TakeHideFilterOff: what it took off is not taken off again at the end; what failed still is.
TEST(HelperSession, TakeHideFilterOffForgetsWhatCameOff) {
    Fake fake;
    auto s = session(fake);
    ASSERT_EQ(s->handle(request(RequestType::PutHideFilter, {"HID\\A", "HID\\B"})).status, Status::Ok);
    fake.takeAnswer = HideSetup::Waiting;
    const Reply waiting = s->handle(request(RequestType::TakeHideFilterOff, {"HID\\A"}));
    EXPECT_EQ(waiting.status, Status::Pending);
    fake.takeAnswer = HideSetup::Failed;
    EXPECT_EQ(s->handle(request(RequestType::TakeHideFilterOff, {"HID\\B"})).status, Status::Failed);
    fake.takeAnswer = HideSetup::Running;
    s.reset();
    ASSERT_EQ(fake.takeCalls.size(), 3U);
    EXPECT_EQ(fake.takeCalls[2], (std::vector<std::string>{"HID\\B"}));
}

// The driver scripts log into ProgramData's folder until the caller shared its own (never SYSTEM's profile).
TEST(HelperSession, ScriptsLogUnderProgramDataWithoutTheCallersFolder) {
    Fake fake;
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::InstallWheelDriver)).status, Status::Ok);
    fake.installWorks = false;
    const Reply failed = s->handle(request(RequestType::InstallWheelDriver));
    EXPECT_EQ(failed.status, Status::Failed);
    EXPECT_EQ(failed.text, "pnputil failed");
    ASSERT_EQ(fake.scriptLogFolders.size(), 2U);
    const std::filesystem::path folder = fake.scriptLogFolders[0];
    EXPECT_EQ(folder.filename(), "logs");
    EXPECT_EQ(folder.parent_path().filename(), "wheelbri");
    wchar_t programData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH) > 0) {
        EXPECT_EQ(folder.parent_path().parent_path(), std::filesystem::path(programData));
    }
}

// ShareLogFolder acts as the caller: without its token it is refused, and nothing is written.
TEST(HelperSession, ShareLogFolderNeedsTheCallersToken) {
    Fake fake;
    const Reply reply = session(fake)->handle(request(RequestType::ShareLogFolder));
    EXPECT_EQ(reply.status, Status::Refused);
    EXPECT_NE(reply.text.find("did not let the helper act as it"), std::string::npos);
}

// SetLogLevel writes the one level it was given.
TEST(HelperSession, SetLogLevelWritesTheLevel) {
    Fake fake;
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::SetLogLevel, {"debug"})).status, Status::Ok);
    ASSERT_EQ(fake.levels.size(), 1U);
    EXPECT_EQ(fake.levels[0], LogLevel::Debug);
}

// CreateVirtualWheel for a wheel the helper does not make: refused, no device; RemoveVirtualWheel without one is Ok.
TEST(HelperSession, AnUnknownVirtualWheelIsRefused) {
    Fake fake;
    auto s = session(fake);
    EXPECT_EQ(s->handle(request(RequestType::CreateVirtualWheel, {"g29"})).status, Status::Refused);
    EXPECT_EQ(s->handle(request(RequestType::RemoveVirtualWheel)).status, Status::Ok);
}

}  // namespace
}  // namespace wheelbri::helper
