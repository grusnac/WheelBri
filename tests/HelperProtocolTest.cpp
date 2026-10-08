#include "wheelbri/HelperProtocol.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace wheelbri::helper;
using wheelbri::LogLevel;
using wheelbri::parseLogLevel;

namespace {

const std::string kMoza = R"(HID\VID_346E&PID_0005&MI_02\8&8052F69&0&0000)";

Request roundTrip(const Request& request) {
    std::string error;
    const auto bytes = encode(request);
    auto decoded = decodeRequest(bytes, error);
    EXPECT_TRUE(decoded.has_value()) << error;
    return decoded.value_or(Request{});
}

}  // namespace

TEST(HelperProtocol, RequestRoundTrip) {
    const Request in{RequestType::PutHideFilter, 0x01020304, {kMoza, R"(HID\VID_0EB7&PID_0E04\7&1&0&0000)"}};
    const Request out = roundTrip(in);
    EXPECT_EQ(out.type, RequestType::PutHideFilter);
    EXPECT_EQ(out.seq, 0x01020304U);
    EXPECT_EQ(out.devices, in.devices);
}

TEST(HelperProtocol, RequestLayout) {
    const auto bytes = encode(Request{RequestType::HideRig, 7, {}});
    const std::vector<std::uint8_t> expected = {'W', 'E', 'H', 'Q', 1, 0, 4, 0, 7, 0, 0, 0, 0};
    EXPECT_EQ(bytes, expected);
}

TEST(HelperProtocol, ReplyRoundTrip) {
    std::string error;
    const auto bytes = encode(Reply{42, Status::Pending, "close the Game Controllers window"});
    const auto reply = decodeReply(bytes, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->seq, 42U);
    EXPECT_EQ(reply->status, Status::Pending);
    EXPECT_EQ(reply->text, "close the Game Controllers window");
}

TEST(HelperProtocol, ReplyTextIsCut) {
    std::string error;
    const auto reply = decodeReply(encode(Reply{1, Status::Failed, std::string(5000, 'x')}), error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->text.size(), kMaxText);
}

TEST(HelperProtocol, RefusesBrokenMessages) {
    std::string error;
    auto bytes = encode(Request{RequestType::PutHideFilter, 1, {kMoza}});

    EXPECT_FALSE(decodeRequest(std::span(bytes).first(5), error).has_value());   // short
    auto truncated = bytes;
    truncated.pop_back();
    EXPECT_FALSE(decodeRequest(truncated, error).has_value());                 // device runs past the end
    auto trailing = bytes;
    trailing.push_back(0);
    EXPECT_FALSE(decodeRequest(trailing, error).has_value());                  // bytes after it
    auto version = bytes;
    version[4] = 2;
    EXPECT_FALSE(decodeRequest(version, error).has_value());
    EXPECT_NE(error.find("version"), std::string::npos);
    auto magic = bytes;
    magic[0] = 'X';
    EXPECT_FALSE(decodeRequest(magic, error).has_value());
    auto reserved = bytes;
    reserved[7] = 1;
    EXPECT_FALSE(decodeRequest(reserved, error).has_value());
    EXPECT_FALSE(decodeRequest(std::vector<std::uint8_t>(kMaxMessage + 1, 0), error).has_value());
    // A reply is not a request, and the other way round.
    EXPECT_FALSE(decodeReply(bytes, error).has_value());
    EXPECT_FALSE(decodeRequest(encode(Reply{}), error).has_value());
}

TEST(HelperProtocol, UnknownTypeDecodesButIsRefused) {
    auto bytes = encode(Request{RequestType::Hello, 1, {}});
    bytes[6] = 99;
    std::string error;
    const auto request = decodeRequest(bytes, error);
    ASSERT_TRUE(request.has_value()) << error;
    EXPECT_FALSE(isKnown(request->type));
    EXPECT_NE(requestProblem(*request), "");
}

TEST(HelperProtocol, RequestRules) {
    EXPECT_EQ(requestProblem(Request{RequestType::Hello, 1, {}}), "");
    EXPECT_EQ(requestProblem(Request{RequestType::HideRig, 1, {}}), "");
    EXPECT_NE(requestProblem(Request{RequestType::HideRig, 1, {kMoza}}), "");   // no devices but for PutHideFilter
    EXPECT_EQ(requestProblem(Request{RequestType::PutHideFilter, 1, {kMoza}}), "");
    EXPECT_NE(requestProblem(Request{RequestType::PutHideFilter, 1, {}}), "");
    EXPECT_NE(requestProblem(Request{RequestType::PutHideFilter, 1, std::vector<std::string>(kMaxDevices + 1, kMoza)}),
              "");
    EXPECT_NE(requestProblem(Request{RequestType::PutHideFilter, 1, {kMoza, "C:\\Windows\\evil.exe"}}), "");
}

TEST(HelperProtocol, HideDeviceRules) {
    EXPECT_EQ(hideDeviceProblem(kMoza), "");
    EXPECT_EQ(hideDeviceProblem(R"(hid\vid_346e&pid_0005&mi_02\8&8052f69&0&0000)"), "");
    EXPECT_NE(hideDeviceProblem(""), "");
    EXPECT_NE(hideDeviceProblem(R"(HID\VID_046D&PID_C262\1&2&0&0000)"), "");   // the virtual G920
    EXPECT_NE(hideDeviceProblem(R"(USB\VID_346E&PID_0005\123)"), "");         // not HID
    EXPECT_NE(hideDeviceProblem(R"(HID\VID_346E&PID_0005)"), "");             // two parts
    EXPECT_NE(hideDeviceProblem(R"(HID\VID_346E&PID_0005\a\b)"), "");         // four parts
    EXPECT_NE(hideDeviceProblem(R"(HID\VID_346E&PID_0005\)"), "");            // empty instance
    EXPECT_NE(hideDeviceProblem(R"(HID\VID_346E&PID_0005\x y)"), "");         // space
    EXPECT_NE(hideDeviceProblem(R"(HID\VID_346E&PID_0005\x"y)"), "");         // quote
    EXPECT_NE(hideDeviceProblem("HID\\VID_346E&PID_0005\\" + std::string(kMaxIdLength, 'A')), "");
}

TEST(HelperProtocol, InstalledApp) {
    const std::string folder = R"(C:\Program Files\WheelBri)";
    EXPECT_TRUE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri.exe)", folder));
    EXPECT_TRUE(isInstalledApp(R"(\\?\c:\program files\wheelbri\WHEELBRI.EXE)", folder + "\\"));
    EXPECT_TRUE(isInstalledApp("C:/Program Files/WheelBri/wheelbri.exe", folder));
    EXPECT_TRUE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri-service.exe)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri-helper.exe)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Users\me\wheelbri.exe)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\sub\wheelbri.exe)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\..\WheelBri\wheelbri.exe)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\.\wheelbri.exe)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri.exe)", ""));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBriX\wheelbri.exe)", folder));
}

TEST(HelperProtocol, Names) {
    EXPECT_STREQ(requestName(RequestType::PutHideFilter), "PutHideFilter");
    EXPECT_STREQ(statusName(Status::Pending), "pending");
}

// --- every remaining branch: the edges of each rule ---

TEST(HelperProtocol, ARequestOfOnlyAHeaderHasNoDeviceCount) {
    auto bytes = encode(Request{RequestType::HideRig, 1, {}});
    bytes.pop_back();   // the device count
    ASSERT_EQ(bytes.size(), kHeaderSize);
    std::string error;
    EXPECT_FALSE(decodeRequest(bytes, error).has_value());
    EXPECT_EQ(error, "no device count");
}

TEST(HelperProtocol, ADeviceWhoseLengthIsCutOffIsRefused) {
    auto bytes = encode(Request{RequestType::PutHideFilter, 1, {}});
    bytes.back() = 1;      // one device...
    bytes.push_back(5);    // ...and only one byte of its length
    std::string error;
    EXPECT_FALSE(decodeRequest(bytes, error).has_value());
    EXPECT_EQ(error, "device 1 of 1 runs past the end");
}

TEST(HelperProtocol, AMessageOfExactlyTheLimitIsRead) {
    // The largest message there is: still read. One byte more is refused unread (RefusesBrokenMessages).
    Request request{RequestType::PutHideFilter, 1, {}};
    const std::size_t room = kMaxMessage - kHeaderSize - 1 - 2;
    request.devices.push_back(std::string(room, 'A'));
    const auto bytes = encode(request);
    ASSERT_EQ(bytes.size(), kMaxMessage);
    EXPECT_TRUE(fitsInOneMessage(request));
    std::string error;
    const auto decoded = decodeRequest(bytes, error);
    ASSERT_TRUE(decoded.has_value()) << error;
    EXPECT_EQ(decoded->devices.front().size(), room);
    request.devices.front().push_back('A');
    EXPECT_FALSE(fitsInOneMessage(request));
}

TEST(HelperProtocol, EncodingKeepsAtMost255DevicesAndIdsOf65535Bytes) {
    Request request{RequestType::PutHideFilter, 1, std::vector<std::string>(300, "x")};
    auto bytes = encode(request);
    EXPECT_EQ(bytes[kHeaderSize], 255);
    request.devices = {std::string(70'000, 'y')};
    bytes = encode(request);
    EXPECT_EQ(bytes.size(), kHeaderSize + 1 + 2 + 0xFFFF);
    EXPECT_EQ(bytes[kHeaderSize + 1], 0xFF);
    EXPECT_EQ(bytes[kHeaderSize + 2], 0xFF);
}

TEST(HelperProtocol, RefusesBrokenReplies) {
    std::string error;
    const auto good = encode(Reply{3, Status::Ok, "hi"});

    auto status = good;
    status[6] = static_cast<std::uint8_t>(Status::Unsupported) + 1;
    EXPECT_FALSE(decodeReply(status, error).has_value());
    EXPECT_EQ(error, "unknown status 5");

    auto noLength = good;
    noLength.resize(kHeaderSize + 1);   // half a length
    EXPECT_FALSE(decodeReply(noLength, error).has_value());
    EXPECT_EQ(error, "text runs past the end");

    auto shortText = good;
    shortText.pop_back();
    EXPECT_FALSE(decodeReply(shortText, error).has_value());
    EXPECT_EQ(error, "text runs past the end");

    auto trailing = good;
    trailing.push_back('!');
    EXPECT_FALSE(decodeReply(trailing, error).has_value());
    EXPECT_EQ(error, "1 bytes after the message");

    EXPECT_FALSE(decodeReply(std::span(good).first(kHeaderSize - 1), error).has_value());
    EXPECT_NE(error.find("too short"), std::string::npos);
    EXPECT_FALSE(decodeReply(std::vector<std::uint8_t>(kMaxMessage + 1, 0), error).has_value());
    EXPECT_NE(error.find("too long"), std::string::npos);

    auto version = good;
    version[5] = 1;   // version 257
    EXPECT_FALSE(decodeReply(version, error).has_value());
    EXPECT_EQ(error, "protocol version 257, expected 1");
}

TEST(HelperProtocol, EveryStatusAndAnEmptyTextRoundTrip) {
    for (const Status status : {Status::Ok, Status::Refused, Status::Failed, Status::Pending, Status::Unsupported}) {
        std::string error;
        const auto reply = decodeReply(encode(Reply{9, status, {}}), error);
        ASSERT_TRUE(reply.has_value()) << error;
        EXPECT_EQ(reply->status, status);
        EXPECT_EQ(reply->text, "");
    }
}

TEST(HelperProtocol, KnownTypesAreExactlyHelloToSetLogLevel) {
    EXPECT_FALSE(isKnown(static_cast<RequestType>(0)));
    for (int type = 1; type <= 9; ++type) EXPECT_TRUE(isKnown(static_cast<RequestType>(type))) << type;
    EXPECT_FALSE(isKnown(static_cast<RequestType>(10)));
    EXPECT_FALSE(isKnown(static_cast<RequestType>(255)));
    EXPECT_EQ(requestProblem(Request{static_cast<RequestType>(0), 1, {}}), "unknown request 0");
}

TEST(HelperProtocol, EveryRequestButPutHideFilterTakesNoDevices) {
    for (int type = 2; type <= 8; ++type) {   // Hello and SetLogLevel take a level (below)
        const auto requestType = static_cast<RequestType>(type);
        if (requestType == RequestType::PutHideFilter) continue;
        EXPECT_EQ(requestProblem(Request{requestType, 1, {}}), "") << type;
        EXPECT_EQ(requestProblem(Request{requestType, 1, {kMoza}}),
                  std::string(requestName(requestType)) + " takes no devices");
    }
    EXPECT_EQ(requestProblem(Request{RequestType::PutHideFilter, 1, std::vector<std::string>(kMaxDevices, kMoza)}), "");
}

TEST(HelperProtocol, AHelloMayCarryTheAppsLogLevel) {
    EXPECT_EQ(requestProblem(Request{RequestType::Hello, 1, {}}), "");          // an older app
    EXPECT_EQ(requestProblem(Request{RequestType::Hello, 1, {"debug"}}), "");
    EXPECT_EQ(requestProblem(Request{RequestType::Hello, 1, {kMoza}}), "Hello takes nothing but the app's log level");
    EXPECT_EQ(requestProblem(Request{RequestType::Hello, 1, {"info", "info"}}),
              "Hello takes nothing but the app's log level");
    EXPECT_FALSE(requestLogLevel(Request{RequestType::Hello, 1, {}}).has_value());
    EXPECT_EQ(requestLogLevel(Request{RequestType::Hello, 1, {"trace"}}), LogLevel::Trace);
    // It goes over the pipe as the body's one string.
    std::string error;
    const auto decoded = decodeRequest(encode(Request{RequestType::Hello, 4, {"info"}}), error);
    ASSERT_TRUE(decoded.has_value()) << error;
    EXPECT_EQ(requestLogLevel(*decoded), LogLevel::Info);
}

TEST(HelperProtocol, SetLogLevelTakesExactlyOneAppLevel) {
    for (const char* level : {"trace", "debug", "info", "warn"}) {
        const Request request{RequestType::SetLogLevel, 1, {level}};
        EXPECT_EQ(requestProblem(request), "") << level;
        EXPECT_EQ(requestLogLevel(request), parseLogLevel(level));
    }
    const std::string problem = "SetLogLevel takes one level: trace, debug, info or warn";
    EXPECT_EQ(requestProblem(Request{RequestType::SetLogLevel, 1, {}}), problem);
    EXPECT_EQ(requestProblem(Request{RequestType::SetLogLevel, 1, {"debug", "info"}}), problem);
    // Quieter than warn would hide warnings and errors: never.
    for (const char* quiet : {"error", "critical", "off"}) {
        EXPECT_EQ(requestProblem(Request{RequestType::SetLogLevel, 1, {quiet}}), problem) << quiet;
    }
    EXPECT_EQ(requestProblem(Request{RequestType::SetLogLevel, 1, {"warning"}}), problem);   // the names it sends
    EXPECT_EQ(requestProblem(Request{RequestType::SetLogLevel, 1, {"DEBUG"}}), problem);
    EXPECT_EQ(requestProblem(Request{RequestType::SetLogLevel, 1, {kMoza}}), problem);
}

TEST(HelperProtocol, HideDeviceEdges) {
    EXPECT_EQ(hideDeviceProblem("HID\\VID_346E&PID_0005\\" + std::string(kMaxIdLength - 22, 'A')), "");   // exactly
    EXPECT_EQ(hideDeviceProblem(R"(HID\VID_346E&PID_0005&Col01\{a-b}.c#d_e)"), "");   // every allowed sign
    EXPECT_EQ(hideDeviceProblem(R"(\VID_346E&PID_0005\1)"),
              "not a device instance id (three parts: HID\\<hardware>\\<instance>)");   // empty first part
    EXPECT_EQ(hideDeviceProblem(R"(HID\\1)"),
              "not a device instance id (three parts: HID\\<hardware>\\<instance>)");   // empty middle part
    EXPECT_EQ(hideDeviceProblem(R"(HIDVID_346E&PID_0005)"),
              "not a device instance id (three parts: HID\\<hardware>\\<instance>)");   // one part
    EXPECT_EQ(hideDeviceProblem(R"(HID\ABC\1)"), "not a HID device with a vendor and product id");
    EXPECT_EQ(hideDeviceProblem(std::string("HID\\VID_346E&PID_0005\\a\0b", 24)),
              "device id with a character not allowed in an instance id");
    EXPECT_EQ(hideDeviceProblem("HID\\VID_346E&PID_0005\\\xC3\xA9"),
              "device id with a character not allowed in an instance id");
}

TEST(HelperProtocol, InstalledAppEdges) {
    const std::string folder = R"(C:\Program Files\WheelBri)";
    EXPECT_TRUE(isInstalledApp("//?/C:/Program Files/WheelBri/wheelbri.exe", folder));
    EXPECT_TRUE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri.exe)", "C:/Program Files/WheelBri//"));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri.exe)", R"(C:\Program Files\..\WheelBri)"));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri.exe)", "\\"));   // only a separator: empty
    EXPECT_FALSE(isInstalledApp("", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri.exe.bak)", folder));
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\xwheelbri.exe)", folder));
    EXPECT_TRUE(isInstalledApp(R"(C:\Program Files\WheelBri\..foo\wheelbri.exe)",
                               R"(C:\Program Files\WheelBri\..foo)"));   // "..foo" is a name, not ".."
}

TEST(HelperProtocol, WheelBriProgramInAnyFolder) {
    EXPECT_TRUE(isWheelBriProgram(R"(C:\Program Files\WheelBri\wheelbri.exe)"));
    EXPECT_TRUE(isWheelBriProgram(R"(D:\cpp-projects\WheelBri\build\service\WHEELBRI-SERVICE.EXE)"));
    EXPECT_TRUE(isWheelBriProgram("D:/x/WheelBri.exe"));
    EXPECT_TRUE(isWheelBriProgram(R"(\\?\D:\x\wheelbri.exe)"));
    EXPECT_TRUE(isWheelBriProgram("wheelbri.exe"));   // a bare name
    EXPECT_FALSE(isWheelBriProgram(R"(C:\Program Files\WheelBri\wheelbri-helper.exe)"));
    EXPECT_FALSE(isWheelBriProgram(R"(C:\x\notwheelbri.exe)"));
    EXPECT_FALSE(isWheelBriProgram(R"(C:\wheelbri.exe\other.exe)"));
    EXPECT_FALSE(isWheelBriProgram(""));
}

TEST(HelperProtocol, EveryNameAndUnknownOnes) {
    const std::pair<RequestType, const char*> requests[] = {
        {RequestType::Hello, "Hello"},
        {RequestType::CreateVirtualG920, "CreateVirtualG920"},
        {RequestType::RemoveVirtualG920, "RemoveVirtualG920"},
        {RequestType::HideRig, "HideRig"},
        {RequestType::ShowRig, "ShowRig"},
        {RequestType::PutHideFilter, "PutHideFilter"},
        {RequestType::InstallG920Driver, "InstallG920Driver"},
        {RequestType::ShareLogFolder, "ShareLogFolder"},
        {RequestType::SetLogLevel, "SetLogLevel"},
    };
    for (const auto& [type, name] : requests) EXPECT_STREQ(requestName(type), name);
    EXPECT_STREQ(requestName(static_cast<RequestType>(99)), "unknown");
    const std::pair<Status, const char*> statuses[] = {
        {Status::Ok, "ok"}, {Status::Refused, "refused"}, {Status::Failed, "failed"},
        {Status::Pending, "pending"}, {Status::Unsupported, "unsupported"},
    };
    for (const auto& [status, name] : statuses) EXPECT_STREQ(statusName(status), name);
    EXPECT_STREQ(statusName(static_cast<Status>(99)), "unknown");
}
