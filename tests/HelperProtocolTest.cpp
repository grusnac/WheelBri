#include "wheelbri/HelperProtocol.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace wheelbri::helper;

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
    EXPECT_FALSE(isInstalledApp(R"(C:\Program Files\WheelBri\wheelbri-service.exe)", folder));
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
