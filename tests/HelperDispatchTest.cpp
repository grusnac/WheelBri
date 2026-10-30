// HelperDispatch: how the WheelBri Helper answers each message, with a fake session. Every rule of answerMessage
// and mayTakeTheLog has its test, in the order the header lists them.

#include "wheelbri/HelperDispatch.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace wheelbri::helper {
namespace {

constexpr const char* kVersion = "WheelBri Helper 0.9.0";
const std::string kMoza = R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)";

class FakeSession final : public RequestHandler {
public:
    enum class Behaviour { Answer, ThrowStd, ThrowOther };

    Reply handle(const Request& request) override {
        seen.push_back(request);
        switch (behaviour) {
            case Behaviour::ThrowStd: throw std::runtime_error("the disk is on fire");
            case Behaviour::ThrowOther: throw 42;
            case Behaviour::Answer: break;
        }
        return reply;
    }

    Behaviour behaviour = Behaviour::Answer;
    Reply reply{0, Status::Ok, "done"};
    std::vector<Request> seen;
};

CallerInfo accepted() {
    return CallerInfo{1234, R"(C:\Program Files\WheelBri\wheelbri.exe)", true, {}};
}

CallerInfo refused(std::string image = R"(C:\Users\me\evil.exe)") {
    return CallerInfo{666, std::move(image), false, "only WheelBri may ask"};
}

Answer ask(const Request& request, const CallerInfo& caller, RequestHandler* session) {
    return answerMessage(encode(request), caller, kVersion, session);
}

// --- a message that cannot be read ---

TEST(HelperDispatch, AnUnreadableMessageIsRefusedAndClosesTheConnection) {
    FakeSession session;
    const std::vector<std::uint8_t> junk = {1, 2, 3};
    const Answer answer = answerMessage(junk, accepted(), kVersion, &session);
    EXPECT_EQ(answer.reply.seq, 0U);
    EXPECT_EQ(answer.reply.status, Status::Refused);
    EXPECT_EQ(answer.reply.text, "unreadable message: message too short (3 bytes)");
    EXPECT_FALSE(answer.keepOpen);
    EXPECT_FALSE(answer.type.has_value());
    EXPECT_TRUE(session.seen.empty());
}

TEST(HelperDispatch, AnEmptyMessageIsUnreadable) {
    const Answer answer = answerMessage({}, refused(), kVersion, nullptr);
    EXPECT_EQ(answer.reply.status, Status::Refused);
    EXPECT_FALSE(answer.keepOpen);
}

// --- Hello ---

TEST(HelperDispatch, HelloFromWheelBriSaysTheVersionAndKeepsTheConnection) {
    FakeSession session;
    const Answer answer = ask(Request{RequestType::Hello, 7, {}}, accepted(), &session);
    EXPECT_EQ(answer.reply.seq, 7U);
    EXPECT_EQ(answer.reply.status, Status::Ok);
    EXPECT_EQ(answer.reply.text, kVersion);
    EXPECT_TRUE(answer.keepOpen);
    EXPECT_EQ(answer.type, RequestType::Hello);
    EXPECT_TRUE(session.seen.empty());   // Hello never reaches the session
}

TEST(HelperDispatch, HelloFromAnyoneElseSaysWhyAndClosesTheConnection) {
    const Answer answer = ask(Request{RequestType::Hello, 8, {}}, refused(), nullptr);
    EXPECT_EQ(answer.reply.seq, 8U);
    EXPECT_EQ(answer.reply.status, Status::Refused);
    EXPECT_EQ(answer.reply.text, std::string(kVersion) + ": only WheelBri may ask");
    EXPECT_FALSE(answer.keepOpen);   // it may not hold one of the helper's few connections
    EXPECT_EQ(answer.type, RequestType::Hello);
}

TEST(HelperDispatch, HelloWithDevicesIsStillHello) {
    // requestProblem would refuse devices on Hello, but Hello is answered before any rule: it only says who is there.
    const Answer answer = ask(Request{RequestType::Hello, 1, {kMoza}}, accepted(), nullptr);
    EXPECT_EQ(answer.reply.status, Status::Ok);
    EXPECT_FALSE(answer.logLevel.has_value());   // not a level
}

TEST(HelperDispatch, AHellosLogLevelIsTakenFromWheelBriOnly) {
    EXPECT_EQ(ask(Request{RequestType::Hello, 1, {"debug"}}, accepted(), nullptr).logLevel, LogLevel::Debug);
    // WheelBri from another folder: refused, closed, but its user's log and level are the helper's.
    const Answer dev = ask(Request{RequestType::Hello, 1, {"trace"}},
                           refused(R"(D:\projects\WheelBri\build\service\wheelbri.exe)"), nullptr);
    EXPECT_EQ(dev.reply.status, Status::Refused);
    EXPECT_EQ(dev.logLevel, LogLevel::Trace);
    // Any other program: nothing.
    EXPECT_FALSE(ask(Request{RequestType::Hello, 1, {"trace"}}, refused(), nullptr).logLevel.has_value());
    // An older app's Hello, or one saying a level the app cannot pick: the level stays.
    EXPECT_FALSE(ask(Request{RequestType::Hello, 1, {}}, accepted(), nullptr).logLevel.has_value());
    EXPECT_FALSE(ask(Request{RequestType::Hello, 1, {"off"}}, accepted(), nullptr).logLevel.has_value());
}

// --- anything else from a caller not accepted ---

TEST(HelperDispatch, ARequestFromACallerNotAcceptedIsRefusedAndClosed) {
    FakeSession session;
    for (int type = 2; type <= 8; ++type) {
        const Answer answer = ask(Request{static_cast<RequestType>(type), 5, {}}, refused(), &session);
        EXPECT_EQ(answer.reply.status, Status::Refused) << type;
        EXPECT_EQ(answer.reply.text, "only WheelBri may ask");
        EXPECT_EQ(answer.reply.seq, 5U);
        EXPECT_FALSE(answer.keepOpen);
    }
    EXPECT_TRUE(session.seen.empty());
}

TEST(HelperDispatch, AnUnknownRequestFromACallerNotAcceptedIsRefusedNotUnsupported) {
    auto bytes = encode(Request{RequestType::Hello, 1, {}});
    bytes[6] = 77;
    const Answer answer = answerMessage(bytes, refused(), kVersion, nullptr);
    EXPECT_EQ(answer.reply.status, Status::Refused);
    EXPECT_FALSE(answer.keepOpen);
}

// --- an unknown request type ---

TEST(HelperDispatch, AnUnknownRequestIsUnsupportedAndTheConnectionStays) {
    FakeSession session;
    auto bytes = encode(Request{RequestType::Hello, 4, {}});
    bytes[6] = 77;   // a request from a newer app
    const Answer answer = answerMessage(bytes, accepted(), kVersion, &session);
    EXPECT_EQ(answer.reply.status, Status::Unsupported);
    EXPECT_EQ(answer.reply.seq, 4U);
    EXPECT_EQ(answer.reply.text, std::string(kVersion) + " does not know this request");
    EXPECT_TRUE(answer.keepOpen);
    EXPECT_TRUE(session.seen.empty());
}

TEST(HelperDispatch, TypeZeroIsUnknownToo) {
    auto bytes = encode(Request{RequestType::Hello, 1, {}});
    bytes[6] = 0;
    EXPECT_EQ(answerMessage(bytes, accepted(), kVersion, nullptr).reply.status, Status::Unsupported);
}

// --- a request the rules refuse ---

TEST(HelperDispatch, ARequestTheRulesRefuseIsRefusedAndTheConnectionStays) {
    FakeSession session;
    Answer answer = ask(Request{RequestType::PutHideFilter, 3, {R"(HID\VID_046D&PID_C262\1&2&0&0000)"}}, accepted(),
                        &session);
    EXPECT_EQ(answer.reply.status, Status::Refused);
    EXPECT_EQ(answer.reply.text, "the virtual wheel (G920, G25) is never hidden");
    EXPECT_TRUE(answer.keepOpen);
    answer = ask(Request{RequestType::HideRig, 4, {kMoza}}, accepted(), &session);
    EXPECT_EQ(answer.reply.text, "HideRig takes no devices");
    answer = ask(Request{RequestType::PutHideFilter, 5, {}}, accepted(), &session);
    EXPECT_EQ(answer.reply.text, "PutHideFilter without devices");
    EXPECT_TRUE(session.seen.empty());
}

// --- carried out by the session ---

TEST(HelperDispatch, AnAllowedRequestGoesToTheSessionAndItsReplyBack) {
    FakeSession session;
    session.reply = Reply{999, Status::Ok, "SWD\\wheelbri\\G920_0"};   // whatever seq it puts: the request's goes back
    const Answer answer = ask(Request{RequestType::CreateVirtualWheel, 11, {}}, accepted(), &session);
    ASSERT_EQ(session.seen.size(), 1U);
    EXPECT_EQ(session.seen[0].type, RequestType::CreateVirtualWheel);
    EXPECT_EQ(answer.reply.seq, 11U);
    EXPECT_EQ(answer.reply.status, Status::Ok);
    EXPECT_EQ(answer.reply.text, "SWD\\wheelbri\\G920_0");
    EXPECT_TRUE(answer.keepOpen);
    EXPECT_EQ(answer.type, RequestType::CreateVirtualWheel);
}

TEST(HelperDispatch, CreateVirtualWheelReachesTheSessionWithItsWheel) {
    FakeSession session;
    const Answer answer = ask(Request{RequestType::CreateVirtualWheel, 12, {"g25"}}, accepted(), &session);
    ASSERT_EQ(session.seen.size(), 1U);
    EXPECT_EQ(session.seen[0].devices, std::vector<std::string>{"g25"});
    EXPECT_EQ(answer.reply.status, Status::Ok);
}

TEST(HelperDispatch, AWheelWheelBriDoesNotEmulateNeverReachesTheSession) {
    FakeSession session;
    for (const std::vector<std::string>& devices :
         {std::vector<std::string>{"g29"}, std::vector<std::string>{""}, std::vector<std::string>{"g25", "g25"}}) {
        const Answer answer = ask(Request{RequestType::CreateVirtualWheel, 13, devices}, accepted(), &session);
        EXPECT_EQ(answer.reply.status, Status::Refused);
        EXPECT_EQ(answer.reply.text, "CreateVirtualWheel takes one wheel: g25, g27, g920 or g923");
    }
    EXPECT_TRUE(session.seen.empty());
}

TEST(HelperDispatch, PutHideFilterReachesTheSessionWithItsDevices) {
    FakeSession session;
    const Answer answer = ask(Request{RequestType::PutHideFilter, 2, {kMoza}}, accepted(), &session);
    ASSERT_EQ(session.seen.size(), 1U);
    EXPECT_EQ(session.seen[0].devices, std::vector<std::string>{kMoza});
    EXPECT_EQ(answer.reply.status, Status::Ok);
}

TEST(HelperDispatch, EveryStatusTheSessionGivesGoesBackAsItIs) {
    FakeSession session;
    for (const Status status : {Status::Ok, Status::Refused, Status::Failed, Status::Pending, Status::Unsupported}) {
        session.reply = Reply{0, status, "text"};
        const Answer answer = ask(Request{RequestType::InstallWheelDriver, 1, {}}, accepted(), &session);
        EXPECT_EQ(answer.reply.status, status);
        EXPECT_EQ(answer.reply.text, "text");
        EXPECT_TRUE(answer.keepOpen);
    }
}

TEST(HelperDispatch, NoSessionIsAFailureNotACrash) {
    const Answer answer = ask(Request{RequestType::ShowRig, 6, {}}, accepted(), nullptr);
    EXPECT_EQ(answer.reply.status, Status::Failed);
    EXPECT_EQ(answer.reply.text, "the helper has no session for this connection");
    EXPECT_EQ(answer.reply.seq, 6U);
    EXPECT_TRUE(answer.keepOpen);
}

TEST(HelperDispatch, ASessionThatThrowsIsAFailureAndTheHelperGoesOn) {
    FakeSession session;
    session.behaviour = FakeSession::Behaviour::ThrowStd;
    Answer answer = ask(Request{RequestType::CreateVirtualWheel, 9, {}}, accepted(), &session);
    EXPECT_EQ(answer.reply.status, Status::Failed);
    EXPECT_EQ(answer.reply.text, "the helper failed: the disk is on fire");
    EXPECT_EQ(answer.reply.seq, 9U);
    EXPECT_TRUE(answer.keepOpen);

    session.behaviour = FakeSession::Behaviour::ThrowOther;
    answer = ask(Request{RequestType::CreateVirtualWheel, 10, {}}, accepted(), &session);
    EXPECT_EQ(answer.reply.status, Status::Failed);
    EXPECT_EQ(answer.reply.text, "the helper failed (an unknown error)");

    session.behaviour = FakeSession::Behaviour::Answer;   // and the next request is served again
    answer = ask(Request{RequestType::CreateVirtualWheel, 11, {}}, accepted(), &session);
    EXPECT_EQ(answer.reply.status, Status::Ok);
}

TEST(HelperDispatch, ASetLogLevelTheSessionCarriedOutSetsTheHelpersLevelToo) {
    FakeSession session;
    const Answer answer = ask(Request{RequestType::SetLogLevel, 3, {"info"}}, accepted(), &session);
    EXPECT_EQ(answer.reply.status, Status::Ok);
    EXPECT_EQ(answer.logLevel, LogLevel::Info);
    ASSERT_EQ(session.seen.size(), 1U);
    EXPECT_EQ(session.seen[0].devices, std::vector<std::string>{"info"});
    // Failed (the registry could not be written): the helper's level stays.
    session.reply = Reply{0, Status::Failed, "access denied"};
    EXPECT_FALSE(ask(Request{RequestType::SetLogLevel, 4, {"debug"}}, accepted(), &session).logLevel.has_value());
    // Refused by the rules: never reaches the session.
    const Answer quiet = ask(Request{RequestType::SetLogLevel, 5, {"off"}}, accepted(), &session);
    EXPECT_EQ(quiet.reply.status, Status::Refused);
    EXPECT_FALSE(quiet.logLevel.has_value());
    EXPECT_EQ(session.seen.size(), 2U);
    // From a caller not accepted: refused and closed.
    const Answer other = ask(Request{RequestType::SetLogLevel, 6, {"debug"}},
                             refused(R"(D:\projects\WheelBri\build\service\wheelbri.exe)"), &session);
    EXPECT_EQ(other.reply.status, Status::Refused);
    EXPECT_FALSE(other.keepOpen);
    EXPECT_FALSE(other.logLevel.has_value());
}

// --- whose folder the helper's log may go to ---

TEST(HelperDispatch, TheLogGoesToWheelBriOnly) {
    EXPECT_TRUE(mayTakeTheLog(accepted()));
    // WheelBri from another folder (a development build, an app run as Administrator): its user's folder too.
    EXPECT_TRUE(mayTakeTheLog(refused(R"(D:\projects\WheelBri\build\service\wheelbri.exe)")));
    EXPECT_TRUE(mayTakeTheLog(refused(R"(D:\x\wheelbri-service.exe)")));
    EXPECT_FALSE(mayTakeTheLog(refused()));
    EXPECT_FALSE(mayTakeTheLog(refused("")));   // a program Windows would not name
    EXPECT_FALSE(mayTakeTheLog(refused(R"(C:\Program Files\WheelBri\wheelbri-helper.exe)")));
}

}  // namespace
}  // namespace wheelbri::helper
