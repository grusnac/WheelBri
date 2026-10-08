// The WheelBri Helper's pipe end to end, Windows only: HelperServer and HelperClient on a pipe of the tests' own
// (never the real helper's), with a fake session, plus HelperLog's file written as this user. Real pipes, threads
// and files; nothing here needs Administrator.

#include "HelperClient.hpp"
#include "HelperLog.hpp"
#include "HelperServer.hpp"

#include "wheelbri/HelperProtocol.hpp"
#include "wheelbri/LiveLog.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wheelbri::helper {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr const char* kVersion = "WheelBri Helper 9.9.9-test";

bool eventually(const std::function<bool()>& condition, const std::chrono::milliseconds limit = 5s) {
    const auto end = Clock::now() + limit;
    while (Clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return condition();
}

std::wstring uniquePipeName() {
    static std::atomic<int> counter{0};
    return L"\\\\.\\pipe\\wheelbri-helper-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(++counter);
}

// What the fake sessions saw, shared with the test.
struct Record {
    std::mutex mutex;
    std::vector<RequestType> requests;
    int made = 0;
    int destroyed = 0;
    int tokens = 0;          // sessions given the caller's token
    std::vector<std::wstring> logFolders;
    bool throwOnRequest = false;

    int madeNow() {
        const std::lock_guard<std::mutex> lock(mutex);
        return made;
    }
    int destroyedNow() {
        const std::lock_guard<std::mutex> lock(mutex);
        return destroyed;
    }
    std::size_t requestsNow() {
        const std::lock_guard<std::mutex> lock(mutex);
        return requests.size();
    }
    std::size_t logsNow() {
        const std::lock_guard<std::mutex> lock(mutex);
        return logFolders.size();
    }
};

class FakeSession final : public RequestHandler {
public:
    FakeSession(Record& record, void* const process, void* const token) : record_(record), process_(process), token_(token) {
        const std::lock_guard<std::mutex> lock(record_.mutex);
        ++record_.made;
        if (token_ != nullptr) ++record_.tokens;
    }
    ~FakeSession() override {
        if (token_ != nullptr) CloseHandle(static_cast<HANDLE>(token_));
        CloseHandle(static_cast<HANDLE>(process_));
        const std::lock_guard<std::mutex> lock(record_.mutex);
        ++record_.destroyed;
    }
    FakeSession(const FakeSession&) = delete;
    FakeSession& operator=(const FakeSession&) = delete;

    Reply handle(const Request& request) override {
        bool throwIt = false;
        {
            const std::lock_guard<std::mutex> lock(record_.mutex);
            record_.requests.push_back(request.type);
            throwIt = record_.throwOnRequest;
        }
        if (throwIt) throw std::runtime_error("session broke");
        return Reply{request.seq, Status::Ok, "done " + std::string(requestName(request.type))};
    }

private:
    Record& record_;
    void* process_;
    void* token_;
};

// A HelperServer on a pipe of its own, served on a thread until the end of the test.
class TestServer {
public:
    explicit TestServer(const bool acceptCaller = true, const int firstMessageMs = HelperServer::kFirstMessageMs,
                        const bool sessions = true)
        : name_(uniquePipeName()), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        HelperServer::Options options;
        options.appFolder = L"C:\\nowhere";
        options.version = kVersion;
        options.pipeName = name_;
        options.firstMessageMs = firstMessageMs;
        options.isApp = [acceptCaller](const std::string&) { return acceptCaller; };
        if (sessions) {
            options.makeSession = [this](void* process, void* token, std::function<bool()>) {
                return std::make_unique<FakeSession>(record_, process, token);
            };
        }
        options.takeLog = [this](void* token, const std::wstring& folder) {
            CloseHandle(static_cast<HANDLE>(token));
            const std::lock_guard<std::mutex> lock(record_.mutex);
            record_.logFolders.push_back(folder);
        };
        server_ = std::make_unique<HelperServer>(std::move(options));
        thread_ = std::thread([this] { ran_ = server_->run(stop_, error_); });
        // Listening once the pipe exists.
        EXPECT_TRUE(eventually([this] { return WaitNamedPipeW(name_.c_str(), 1) || GetLastError() != ERROR_FILE_NOT_FOUND; }));
    }
    ~TestServer() { stop(); }
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    void stop() {
        if (!thread_.joinable()) return;
        SetEvent(stop_);
        thread_.join();
        CloseHandle(stop_);
    }

    [[nodiscard]] const std::wstring& name() const { return name_; }
    Record& record() { return record_; }
    [[nodiscard]] bool ran() const { return ran_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    std::wstring name_;
    HANDLE stop_;
    Record record_;
    std::unique_ptr<HelperServer> server_;
    std::thread thread_;
    bool ran_ = false;
    std::string error_;
};

// The pipe opened without HelperClient, for what the client never sends.
HANDLE openRaw(const std::wstring& name) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        // The server makes its next instance a moment after the last one was taken.
        if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 2000);
    }
    if (pipe == INVALID_HANDLE_VALUE) return pipe;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    return pipe;
}

bool writeRaw(const HANDLE pipe, const std::vector<std::uint8_t>& bytes) {
    DWORD written = 0;
    return WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE;
}

// Reads one reply; nullopt when the server closed the connection instead.
std::optional<Reply> readRaw(const HANDLE pipe) {
    std::vector<std::uint8_t> buffer(kMaxMessage);
    DWORD read = 0;
    if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return std::nullopt;
    std::string error;
    return decodeReply(std::span<const std::uint8_t>(buffer.data(), read), error);
}

std::optional<Reply> call(HelperClient& client, const RequestType type, std::string& error,
                          const std::vector<std::string>& devices = {}) {
    return client.call(type, devices, 5000, error);
}

// --- Hello, and who may ask ---

TEST(HelperPipe, WheelBriSaysHelloAndThenAsks) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    EXPECT_EQ(client.serverPid(), GetCurrentProcessId());
    auto reply = call(client, RequestType::Hello, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Ok);
    EXPECT_EQ(reply->text, kVersion);
    reply = call(client, RequestType::CreateVirtualG920, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Ok);
    EXPECT_EQ(reply->text, "done CreateVirtualG920");
    EXPECT_EQ(server.record().madeNow(), 1);
    EXPECT_EQ(server.record().requestsNow(), 1U);   // Hello never reaches the session
}

TEST(HelperPipe, AnAcceptedCallersSessionGetsItsTokenAndTheLogMoves) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    ASSERT_TRUE(call(client, RequestType::Hello, error).has_value()) << error;
    ASSERT_TRUE(eventually([&] { return server.record().logsNow() == 1; }));
    const std::lock_guard<std::mutex> lock(server.record().mutex);
    EXPECT_EQ(server.record().tokens, 1);
    const std::wstring& folder = server.record().logFolders.front();
    EXPECT_EQ(folder.substr(folder.size() - 14), L"\\wheelbri\\logs");
}

TEST(HelperPipe, AnyOtherProgramMayOnlySayHelloAndIsThenClosed) {
    TestServer server(/*acceptCaller=*/false);
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    auto reply = call(client, RequestType::Hello, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Refused);
    EXPECT_EQ(reply->text.rfind(std::string(kVersion) + ": only WheelBri", 0), 0U) << reply->text;
    // The helper closed the connection: the next call finds it broken.
    reply = call(client, RequestType::Hello, error);
    EXPECT_FALSE(reply.has_value());
    EXPECT_EQ(server.record().madeNow(), 0);
    // The tests' program is not WheelBri: the log stays where it is.
    EXPECT_EQ(server.record().logsNow(), 0U);
}

TEST(HelperPipe, ARequestFromAnyOtherProgramIsRefusedAndClosed) {
    TestServer server(/*acceptCaller=*/false);
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    const auto reply = call(client, RequestType::HideRig, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Refused);
    EXPECT_FALSE(call(client, RequestType::Hello, error).has_value());
    EXPECT_EQ(server.record().requestsNow(), 0U);
}

// --- the rules on the pipe ---

TEST(HelperPipe, ARequestTheRulesRefuseKeepsTheConnection) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    auto reply = call(client, RequestType::PutHideFilter, error, {"C:\\Windows\\evil.exe"});
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Refused);
    reply = call(client, RequestType::ShowRig, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Ok);
}

TEST(HelperPipe, AnUnknownRequestIsUnsupportedAndTheConnectionStays) {
    TestServer server;
    const HANDLE pipe = openRaw(server.name());
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    auto bytes = encode(Request{RequestType::Hello, 5, {}});
    bytes[6] = 42;
    ASSERT_TRUE(writeRaw(pipe, bytes));
    auto reply = readRaw(pipe);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, Status::Unsupported);
    EXPECT_EQ(reply->seq, 5U);
    ASSERT_TRUE(writeRaw(pipe, encode(Request{RequestType::Hello, 6, {}})));
    reply = readRaw(pipe);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, Status::Ok);
    CloseHandle(pipe);
}

TEST(HelperPipe, AnUnreadableMessageIsRefusedAndClosed) {
    TestServer server;
    const HANDLE pipe = openRaw(server.name());
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(writeRaw(pipe, {1, 2, 3, 4}));
    const auto reply = readRaw(pipe);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(reply->status, Status::Refused);
    EXPECT_EQ(reply->seq, 0U);
    EXPECT_FALSE(readRaw(pipe).has_value());   // closed
    CloseHandle(pipe);
}

TEST(HelperPipe, AMessageLongerThanTheLimitClosesTheConnectionUnread) {
    TestServer server;
    const HANDLE pipe = openRaw(server.name());
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    writeRaw(pipe, std::vector<std::uint8_t>(kMaxMessage + 1, 0x41));
    EXPECT_FALSE(readRaw(pipe).has_value());
    CloseHandle(pipe);
}

TEST(HelperPipe, ASessionThatThrowsFailsTheRequestAndTheHelperGoesOn) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    {
        const std::lock_guard<std::mutex> lock(server.record().mutex);
        server.record().throwOnRequest = true;
    }
    auto reply = call(client, RequestType::InstallG920Driver, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Failed);
    EXPECT_EQ(reply->text, "the helper failed: session broke");
    {
        const std::lock_guard<std::mutex> lock(server.record().mutex);
        server.record().throwOnRequest = false;
    }
    reply = call(client, RequestType::InstallG920Driver, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Ok);
}

TEST(HelperPipe, WithoutSessionsEveryRequestButHelloFails) {
    TestServer server(true, HelperServer::kFirstMessageMs, /*sessions=*/false);
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    auto reply = call(client, RequestType::Hello, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Ok);
    reply = call(client, RequestType::HideRig, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Failed);
}

// --- connections ending ---

TEST(HelperPipe, ClosingTheConnectionEndsItsSession) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    ASSERT_TRUE(call(client, RequestType::CreateVirtualG920, error).has_value()) << error;
    EXPECT_EQ(server.record().destroyedNow(), 0);
    client.close();   // Stop in the app, or the app crashing: the helper undoes what it set up
    EXPECT_TRUE(eventually([&] { return server.record().destroyedNow() == 1; }));
}

TEST(HelperPipe, ACallerThatSaysNothingIsDroppedAndItsPlaceFreed) {
    TestServer server(true, /*firstMessageMs=*/300);
    std::vector<HANDLE> silent;
    for (int i = 0; i < HelperServer::kMaxConnections; ++i) {
        const HANDLE pipe = openRaw(server.name());
        ASSERT_NE(pipe, INVALID_HANDLE_VALUE) << i;
        silent.push_back(pipe);
    }
    // Every place is taken; once the silent callers are dropped, WheelBri gets in.
    HelperClient client(server.name());
    std::string error;
    const auto start = Clock::now();
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    EXPECT_LT(Clock::now() - start, 5s);
    EXPECT_TRUE(call(client, RequestType::Hello, error).has_value()) << error;
    for (const HANDLE pipe : silent) {
        EXPECT_FALSE(readRaw(pipe).has_value());   // the helper closed it
        CloseHandle(pipe);
    }
}

TEST(HelperPipe, AtMostFourConnectionsAndAFifthGetsInWhenOneEnds) {
    TestServer server;
    std::vector<std::unique_ptr<HelperClient>> clients;
    std::string error;
    for (int i = 0; i < HelperServer::kMaxConnections; ++i) {
        clients.push_back(std::make_unique<HelperClient>(server.name()));
        ASSERT_EQ(clients.back()->connect(error), HelperClient::Connect::Ok) << i << ": " << error;
        ASSERT_TRUE(call(*clients.back(), RequestType::Hello, error).has_value()) << error;
    }
    std::thread ender([&] {
        std::this_thread::sleep_for(300ms);
        clients.front()->close();
    });
    HelperClient fifth(server.name());
    const auto connected = fifth.connect(error);   // waits up to 2 s for a free place
    ender.join();
    ASSERT_EQ(connected, HelperClient::Connect::Ok) << error;
    EXPECT_TRUE(call(fifth, RequestType::Hello, error).has_value()) << error;
}

TEST(HelperPipe, StoppingEndsEveryConnectionAndSession) {
    TestServer server;
    HelperClient a(server.name());
    HelperClient b(server.name());
    std::string error;
    ASSERT_EQ(a.connect(error), HelperClient::Connect::Ok) << error;
    ASSERT_EQ(b.connect(error), HelperClient::Connect::Ok) << error;
    ASSERT_TRUE(call(a, RequestType::HideRig, error).has_value()) << error;
    ASSERT_TRUE(call(b, RequestType::HideRig, error).has_value()) << error;
    const auto start = Clock::now();
    server.stop();
    EXPECT_LT(Clock::now() - start, 3s);
    EXPECT_TRUE(server.ran());
    EXPECT_EQ(server.record().destroyedNow(), 2);
    EXPECT_FALSE(call(a, RequestType::Hello, error).has_value());
}

// Wine (where these tests also run, from Linux) does not refuse a second "first instance" of a pipe name.
bool underWine() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

TEST(HelperPipe, ASecondHelperOnTheSameNameIsRefused) {
    if (underWine()) GTEST_SKIP() << "Wine does not implement FILE_FLAG_FIRST_PIPE_INSTANCE";
    TestServer server;
    HelperServer::Options options;
    options.pipeName = server.name();
    options.version = kVersion;
    HelperServer second(std::move(options));
    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread guard([stop] {   // should the second one serve after all, the test still ends
        std::this_thread::sleep_for(3s);
        SetEvent(stop);
    });
    std::string error;
    EXPECT_FALSE(second.run(stop, error));
    SetEvent(stop);
    guard.join();
    EXPECT_EQ(error, "the pipe exists already: another helper (or a program posing as one) runs");
    CloseHandle(stop);
}

// --- the client's own checks ---

TEST(HelperPipe, NoHelperIsSaidSo) {
    HelperClient client(uniquePipeName());
    std::string error;
    EXPECT_EQ(client.connect(error), HelperClient::Connect::NoHelper);
    EXPECT_EQ(error, "the WheelBri Helper is not running");
    EXPECT_FALSE(client.call(RequestType::Hello, {}, 1000, error).has_value());
    EXPECT_EQ(error, "not connected to the helper");
}

TEST(HelperPipe, APipeOfAnotherProgramIsRefusedByTheClient) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    EXPECT_EQ(client.connect(error, "C:\\Program Files\\WheelBri"), HelperClient::Connect::Failed);
    EXPECT_NE(error.find("not to C:\\Program Files\\WheelBri\\wheelbri-helper.exe"), std::string::npos) << error;
}

TEST(HelperPipe, ARequestTooLongIsNotSentAndTheConnectionStays) {
    TestServer server;
    HelperClient client(server.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    EXPECT_FALSE(client.call(RequestType::PutHideFilter, {std::string(kMaxMessage, 'A')}, 1000, error).has_value());
    EXPECT_NE(error.find("longer than the helper reads"), std::string::npos) << error;
    EXPECT_TRUE(call(client, RequestType::Hello, error).has_value()) << error;
}

// A pipe the test answers by hand: what a broken or posing helper would send.
class HandServer {
public:
    HandServer() : name_(uniquePipeName()) {
        pipe_ = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                 1, 8192, 8192, 0, nullptr);
    }
    ~HandServer() {
        if (thread_.joinable()) thread_.join();
        CloseHandle(pipe_);
    }
    HandServer(const HandServer&) = delete;
    HandServer& operator=(const HandServer&) = delete;

    // Accepts one client, reads one message and answers with what `answer` makes of it (nothing when empty).
    void serve(std::function<std::vector<std::uint8_t>(const Request&)> answer) {
        thread_ = std::thread([this, answer = std::move(answer)] {
            ConnectNamedPipe(pipe_, nullptr);
            std::vector<std::uint8_t> buffer(kMaxMessage);
            DWORD read = 0;
            if (!ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return;
            std::string error;
            const auto request = decodeRequest(std::span<const std::uint8_t>(buffer.data(), read), error);
            const std::vector<std::uint8_t> bytes = answer(request.value_or(Request{}));
            if (!bytes.empty()) {
                DWORD written = 0;
                WriteFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            }
            // Wait for the client to go before the pipe is closed.
            ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
        });
    }
    [[nodiscard]] const std::wstring& name() const { return name_; }

private:
    std::wstring name_;
    HANDLE pipe_;
    std::thread thread_;
};

TEST(HelperPipe, AHelperThatDoesNotAnswerInTimeIsGivenUp) {
    HandServer hand;
    hand.serve([](const Request&) { return std::vector<std::uint8_t>{}; });
    HelperClient client(hand.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    const auto start = Clock::now();
    EXPECT_FALSE(client.call(RequestType::Hello, {}, 300, error).has_value());
    EXPECT_EQ(error, "the helper did not answer within 300 ms");
    EXPECT_LT(Clock::now() - start, 3s);
    EXPECT_FALSE(client.call(RequestType::Hello, {}, 300, error).has_value());   // the connection was closed
    EXPECT_EQ(error, "not connected to the helper");
}

TEST(HelperPipe, AWaitTheAppCancelsEndsAtOnce) {
    HandServer hand;
    hand.serve([](const Request&) { return std::vector<std::uint8_t>{}; });
    HelperClient client(hand.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    EXPECT_FALSE(client.call(RequestType::Hello, {}, 60'000, error, [] { return true; }).has_value());
    EXPECT_EQ(error, "stopped");
}

TEST(HelperPipe, AnAnswerToAnotherRequestIsRefused) {
    HandServer hand;
    hand.serve([](const Request& request) { return encode(Reply{request.seq + 7, Status::Ok, "x"}); });
    HelperClient client(hand.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    EXPECT_FALSE(client.call(RequestType::Hello, {}, 3000, error).has_value());
    EXPECT_EQ(error, "the helper answered another request");
}

TEST(HelperPipe, AnAnswerWithSequenceZeroIsTaken) {
    // The helper answers an unreadable message with seq 0: the client takes it as the answer.
    HandServer hand;
    hand.serve([](const Request&) { return encode(Reply{0, Status::Refused, "unreadable message: x"}); });
    HelperClient client(hand.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    const auto reply = client.call(RequestType::Hello, {}, 3000, error);
    ASSERT_TRUE(reply.has_value()) << error;
    EXPECT_EQ(reply->status, Status::Refused);
}

TEST(HelperPipe, AnUnreadableAnswerIsRefused) {
    HandServer hand;
    hand.serve([](const Request&) { return std::vector<std::uint8_t>{'n', 'o', 'p', 'e'}; });
    HelperClient client(hand.name());
    std::string error;
    ASSERT_EQ(client.connect(error), HelperClient::Connect::Ok) << error;
    EXPECT_FALSE(client.call(RequestType::Hello, {}, 3000, error).has_value());
    EXPECT_EQ(error.rfind("unreadable reply from the helper: ", 0), 0U) << error;
}

// --- the helper's log file, written as this user ---

std::filesystem::path tempFolder(const char* name) {
    return std::filesystem::temp_directory_path() /
           (std::string("wheelbri-helperlog-") + name + "-" + std::to_string(GetCurrentProcessId()));
}

HANDLE myImpersonationToken() {
    HANDLE process = nullptr;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process)) return nullptr;
    if (!DuplicateTokenEx(process, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation,
                          &token)) {
        token = nullptr;
    }
    CloseHandle(process);
    return token;
}

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

TEST(HelperLogFile, IsMadeWithItsFoldersAndAppendedToAsTheUser) {
    const std::filesystem::path base = tempFolder("make");
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);   // %LOCALAPPDATA%: always there
    const std::filesystem::path logs = base / "wheelbri" / "logs";   // neither folder there yet (a clean install)
    {
        auto file = HelperLog::userFile(myImpersonationToken(), logs.wstring());
        ASSERT_TRUE(file->open("2026-10-08"));
        EXPECT_TRUE(file->write("one\r\n"));
        file->close();
        EXPECT_FALSE(file->write("closed\r\n"));
        ASSERT_TRUE(file->open("2026-10-08"));   // again: appended, not replaced
        EXPECT_TRUE(file->write("two\r\n"));
        ASSERT_TRUE(file->open("2026-10-09"));   // another day while open: the first is closed
        EXPECT_TRUE(file->write("next day\r\n"));
    }
    EXPECT_EQ(readAll(logs / "wheelbri-helper-2026-10-08.log"), "one\r\ntwo\r\n");
    EXPECT_EQ(readAll(logs / "wheelbri-helper-2026-10-09.log"), "next day\r\n");
    std::filesystem::remove_all(base);
}

TEST(HelperLogFile, WithoutATokenOrAFolderNothingOpens) {
    auto noToken = HelperLog::userFile(nullptr, tempFolder("none").wstring());
    EXPECT_FALSE(noToken->open("2026-10-08"));
    EXPECT_FALSE(noToken->write("x\r\n"));
    auto noFolder = HelperLog::userFile(myImpersonationToken(), L"");
    EXPECT_FALSE(noFolder->open("2026-10-08"));
}

TEST(HelperLogFile, AFolderThatCannotBeMadeDoesNotOpen) {
    const std::filesystem::path base = tempFolder("blocked");
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    std::ofstream(base / "logs") << "a file where the folder should be";
    auto file = HelperLog::userFile(myImpersonationToken(), (base / "logs").wstring());
    EXPECT_FALSE(file->open("2026-10-08"));
    std::filesystem::remove_all(base);
}

TEST(HelperLogFile, ItsLinesReadLikeTheOtherLogs) {
    const std::string line = HelperLog::formatLine(spdlog::level::warn, "helper.pipe", "pid 5 said nothing");
    ASSERT_GE(line.size(), 2U);
    EXPECT_EQ(line.substr(line.size() - 2), "\r\n");
    const LiveLogLine parsed = parseLogLine(line.substr(0, line.size() - 2), 0);   // as LiveLog splits lines
    EXPECT_EQ(parsed.level, LiveLogLevel::Warn);
    EXPECT_EQ(parsed.logger, "helper.pipe");
    EXPECT_EQ(parsed.message, "pid 5 said nothing");
    EXPECT_EQ(parsed.time.size(), 12U);
    // A long component name is kept whole, not cut.
    const LiveLogLine longName =
        parseLogLine(HelperLog::formatLine(spdlog::level::err, "helper.a-very-long-component", "x"), 0);
    EXPECT_EQ(longName.logger, "helper.a-very-long-component");
    EXPECT_EQ(longName.level, LiveLogLevel::Error);
}

}  // namespace
}  // namespace wheelbri::helper
