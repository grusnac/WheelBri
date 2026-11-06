// SPDX-License-Identifier: GPL-3.0-or-later
// wheelbri-logi.dll as a game loads it (logi/src): the built DLL is copied into a folder of its own for each test (each
// copy is a module of its own, with its own settings, log and real library) and its exports are called through
// GetProcAddress, the way NFS Payback does. The "real" Logitech library behind it is FakeLogiLibrary.cpp.
//
// Every test points the places the DLL looks at into its own folder first: %ProgramW6432% (where G HUB and the old
// Logitech software keep their DLLs), %LOCALAPPDATA% and, under Wine, XDG_STATE_HOME / XDG_CONFIG_HOME (where its log
// and the app's level are), so nothing on the machine running the tests is used or written.
#include <windows.h>

#include "wheelbri/LogiSdk.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef WHEELBRI_LOGI_DLL
#error "WHEELBRI_LOGI_DLL: the built wheelbri-logi.dll"
#endif
#ifndef WHEELBRI_FAKE_LOGI_DLL
#error "WHEELBRI_FAKE_LOGI_DLL: the built FakeLogiLibrary.dll"
#endif

namespace {

namespace fs = std::filesystem;
namespace logi = wheelbri::logi;

using Raw = std::int64_t;
using RawFn = Raw (*)(Raw, Raw, Raw, Raw);
using LedsFn = Raw (*)(Raw, float, float, float);

struct FakeLast {   // FakeLogiLibrary.cpp's
    char name[64];
    Raw args[4];
    float floats[3];
    std::uint8_t properties[36];
    int calls;
};
using LastFn = FakeLast* (*)();

void setEnvironment(const wchar_t* name, const std::wstring& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
}

std::wstring environment(const wchar_t* name) {
    wchar_t value[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(value, length) : std::wstring();
}

// The Unix form of a path under Wine ("/tmp/..."), empty elsewhere: for XDG_*, which the DLL reads as Unix paths.
std::wstring unixPath(const fs::path& path) {
    using UnixNameFn = char*(__cdecl*)(const WCHAR*);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto unixName = reinterpret_cast<UnixNameFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "wine_get_unix_file_name")));
    if (unixName == nullptr) return {};
    char* const name = unixName(path.wstring().c_str());
    if (name == nullptr) return {};
    const std::string text(name);
    HeapFree(GetProcessHeap(), 0, name);
    return std::wstring(text.begin(), text.end());
}

class LogiDll : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-logidll-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_ / "game");
        fs::create_directories(dir_ / "programs");
        fs::create_directories(dir_ / "local");
        for (const wchar_t* name : {L"ProgramW6432", L"ProgramFiles", L"LOCALAPPDATA", L"XDG_STATE_HOME",
                                    L"XDG_CONFIG_HOME"}) {
            saved_.emplace_back(name, environment(name));
        }
        setEnvironment(L"ProgramW6432", (dir_ / "programs").wstring());
        setEnvironment(L"LOCALAPPDATA", (dir_ / "local").wstring());
        setEnvironment(L"XDG_STATE_HOME", unixPath(dir_ / "local"));
        setEnvironment(L"XDG_CONFIG_HOME", unixPath(dir_ / "local"));
        fs::copy_file(WHEELBRI_LOGI_DLL, dll(), fs::copy_options::overwrite_existing);
    }

    void TearDown() override {
        if (module_ != nullptr) FreeLibrary(module_);
        if (fake_ != nullptr) FreeLibrary(fake_);
        for (const auto& [name, value] : saved_) setEnvironment(name.c_str(), value);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    [[nodiscard]] fs::path dll() const { return dir_ / "game" / "wheelbri-logi.dll"; }

    void writeIni(const std::string& text) const { std::ofstream(dir_ / "game" / "wheelbri-logi.ini") << text; }

    // A copy of the fake real library at `path` (folders made).
    static void putFake(const fs::path& path) {
        fs::create_directories(path.parent_path());
        fs::copy_file(WHEELBRI_FAKE_LOGI_DLL, path, fs::copy_options::overwrite_existing);
    }

    void load() {
        module_ = LoadLibraryW(dll().c_str());
        ASSERT_NE(module_, nullptr) << GetLastError();
    }

    [[nodiscard]] RawFn fn(const char* name) const {
        return reinterpret_cast<RawFn>(reinterpret_cast<void*>(GetProcAddress(module_, name)));
    }
    Raw call(const char* name, const Raw a = 0, const Raw b = 0, const Raw c = 0, const Raw d = 0) const {
        const RawFn f = fn(name);
        EXPECT_NE(f, nullptr) << name;
        return f != nullptr ? f(a, b, c, d) : -1;
    }

    // The fake library this test's DLL passes calls on to (already loaded by it: the same module).
    [[nodiscard]] FakeLast* last(const fs::path& path) {
        if (fake_ == nullptr) fake_ = LoadLibraryW(path.c_str());
        if (fake_ == nullptr) return nullptr;
        const auto f = reinterpret_cast<LastFn>(reinterpret_cast<void*>(GetProcAddress(fake_, "FakeLogiLast")));
        return f != nullptr ? f() : nullptr;
    }

    // Everything the DLL logged so far (its log is flushed line by line).
    [[nodiscard]] std::string logText() const {
        std::string text;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(dir_ / "local", ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("wheelbri-logi", 0) == 0 && entry.path().extension() == ".log") {
                std::ifstream in(entry.path(), std::ios::binary);
                text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
        }
        return text;
    }

    fs::path dir_;
    HMODULE module_ = nullptr;
    HMODULE fake_ = nullptr;
    std::vector<std::pair<std::wstring, std::wstring>> saved_;
};

// Named in wheelbri-logi.ini, the real library gets every call unchanged and its answers come back unchanged: plain
// values, the state pointer, the text, range and properties written through the game's pointers, and the rev lights'
// floats (passed in XMM registers). A function it lacks answers 0. The log shows each call with what it returned.
TEST_F(LogiDll, PassesEveryCallOnToTheNamedRealLibrary) {
    const fs::path real = dir_ / "sdk" / "real.dll";
    putFake(real);
    writeIni("[logi]\nlog_level = trace\nreal_library = " + real.string() + "\n");
    load();

    EXPECT_EQ(call("LogiSteeringInitialize", 1), 1);
    FakeLast* const seen = last(real);
    ASSERT_NE(seen, nullptr);
    EXPECT_STREQ(seen->name, "LogiSteeringInitialize");
    EXPECT_EQ(seen->args[0], 1);

    EXPECT_EQ(call("LogiIsConnected", 0), 1);
    EXPECT_EQ(call("LogiIsConnected", 1), 0);
    EXPECT_EQ(call("LogiButtonIsPressed", 0, 3), 1);
    EXPECT_EQ(call("LogiGetNonLinearValue", 0, 200), 100);
    EXPECT_EQ(call("LogiPlaySpringForce", 0, 10, 50, 90), 1);
    EXPECT_EQ(seen->args[3], 90);

    const Raw state = call("LogiGetState", 0);
    ASSERT_NE(state, 0);
    std::int32_t x = 0;
    std::memcpy(&x, reinterpret_cast<const void*>(state), sizeof(x));
    EXPECT_EQ(x, 1234);
    EXPECT_EQ(call("LogiGetState", 1), 0);   // no state

    wchar_t name[32] = {};
    EXPECT_EQ(call("LogiGetFriendlyProductName", 0, reinterpret_cast<Raw>(name), 32), 1);
    EXPECT_EQ(std::wstring(name), L"G923 é");
    EXPECT_EQ(call("LogiGetFriendlyProductName", 1, reinterpret_cast<Raw>(name), 32), 0);

    int range = 0;
    EXPECT_EQ(call("LogiGetOperatingRange", 0, reinterpret_cast<Raw>(&range)), 1);
    EXPECT_EQ(range, 900);
    EXPECT_EQ(call("LogiGetOperatingRange", 0, 0), 0);

    std::uint8_t properties[36] = {};
    EXPECT_EQ(call("LogiGetCurrentControllerProperties", 0, reinterpret_cast<Raw>(properties)), 1);
    EXPECT_EQ(properties[0], 1);
    EXPECT_EQ(call("LogiGetCurrentControllerProperties", 1, reinterpret_cast<Raw>(properties)), 0);

    std::uint8_t preferred[36] = {};
    preferred[0] = 1;
    const int gain = 65;
    std::memcpy(preferred + 4, &gain, sizeof(gain));
    EXPECT_EQ(call("LogiSetPreferredControllerProperties", reinterpret_cast<Raw>(preferred)), 1);
    EXPECT_EQ(seen->properties[4], 65);
    EXPECT_EQ(call("LogiSetPreferredControllerProperties", 0), 1);   // no properties

    const auto leds = reinterpret_cast<LedsFn>(reinterpret_cast<void*>(GetProcAddress(module_, "LogiPlayLeds")));
    ASSERT_NE(leds, nullptr);
    EXPECT_EQ(leds(0, 4000.0F, 3000.0F, 7000.0F), 1);
    EXPECT_FLOAT_EQ(seen->floats[0], 4000.0F);
    EXPECT_FLOAT_EQ(seen->floats[1], 3000.0F);
    EXPECT_FLOAT_EQ(seen->floats[2], 7000.0F);

    const int callsBefore = seen->calls;
    EXPECT_EQ(call("LogiGetShifterMode", 0), 0);   // not in the real library
    EXPECT_EQ(seen->calls, callsBefore);
    const auto ledsDInput =
        reinterpret_cast<LedsFn>(reinterpret_cast<void*>(GetProcAddress(module_, "LogiPlayLedsDInput")));
    ASSERT_NE(ledsDInput, nullptr);
    EXPECT_EQ(ledsDInput(0, 1.0F, 2.0F, 3.0F), 0);

    const std::string log = logText();
    EXPECT_NE(log.find("wheelbri-logi.ini names the real library"), std::string::npos) << log;
    EXPECT_NE(log.find("functions found in the real library"), std::string::npos);
    EXPECT_NE(log.find("the real library has no LogiGetShifterMode"), std::string::npos);
    EXPECT_NE(log.find("x=1234"), std::string::npos);
    EXPECT_NE(log.find("no state"), std::string::npos);
    EXPECT_NE(log.find("'G923 ?'"), std::string::npos);
    EXPECT_NE(log.find("range 900 deg"), std::string::npos);
    EXPECT_NE(log.find("overall gain 80%"), std::string::npos);   // what the real one wrote back
    EXPECT_NE(log.find("overall gain 65%"), std::string::npos);   // what the game asked for, read before the call
    EXPECT_NE(log.find("no properties"), std::string::npos);
    EXPECT_NE(log.find("(not in the real library)"), std::string::npos);
    EXPECT_NE(log.find("log level trace (wheelbri-logi.ini)"), std::string::npos);
}

// The same call with the same answer is logged once at info, a changed answer again; the state read every frame is
// logged as "first" once, then only when it changes (debug) or every 1000th call (trace).
TEST_F(LogiDll, RepeatedCallsAreLoggedWhenTheyChange) {
    const fs::path real = dir_ / "sdk" / "real.dll";
    putFake(real);
    writeIni("[logi]\nlog_level = trace\nreal_library = " + real.string() + "\n");
    load();
    for (int i = 0; i < 1000; ++i) call("LogiGetState", 0);
    call("LogiGetState", 1);
    for (int i = 0; i < 5; ++i) call("LogiIsConnected", 0);
    call("LogiIsConnected", 1);
    for (int i = 0; i < 4; ++i) call("LogiPlayConstantForce", 0, 50);
    call("LogiPlayConstantForce", 0, 60);
    const auto leds = reinterpret_cast<LedsFn>(reinterpret_cast<void*>(GetProcAddress(module_, "LogiPlayLeds")));
    for (int i = 0; i < 5; ++i) leds(0, 1000.0F + static_cast<float>(i), 1.0F, 2.0F);

    const std::string log = logText();
    EXPECT_NE(log.find("#1 first LogiGetState"), std::string::npos) << log;
    EXPECT_NE(log.find("#1000 LogiGetState"), std::string::npos);    // sampled
    EXPECT_NE(log.find("#1001 first LogiGetState"), std::string::npos);   // another index: first for it
    EXPECT_EQ(log.find("#500 LogiGetState"), std::string::npos);     // neither
    EXPECT_NE(log.find("#6 LogiIsConnected"), std::string::npos);    // index 1 asked: first for that key
    EXPECT_EQ(log.find("#4 LogiIsConnected"), std::string::npos);
    EXPECT_NE(log.find("#5 LogiPlayConstantForce"), std::string::npos);   // changed force
    EXPECT_NE(log.find("#1 first LogiPlayLeds"), std::string::npos);
    EXPECT_EQ(log.find("#4 LogiPlayLeds"), std::string::npos);
}

// Without a real library every call answers 0 (the game sees no wheel, as without the helper DLL) and the log says
// where it looked: the one wheelbri-logi.ini named (missing), G HUB's folder (empty), the old software's.
TEST_F(LogiDll, WithoutARealLibraryEveryCallAnswersZero) {
    writeIni("[logi]\nlog_level = info\nreal_library = " + (dir_ / "nowhere.dll").string() + "\n");
    load();
    EXPECT_EQ(call("LogiSteeringInitialize", 1), 0);
    EXPECT_EQ(call("LogiGetState", 0), 0);
    wchar_t name[8] = {L'x'};
    EXPECT_EQ(call("LogiGetFriendlyProductName", 0, reinterpret_cast<Raw>(name), 8), 0);
    EXPECT_EQ(name[0], L'x');   // untouched
    // Every export, twice, with nothing behind it and no pointers: each answers 0 and reads nothing.
    for (int round = 0; round < 2; ++round) {
    for (const logi::Function& f : logi::functions()) {
        const std::string name(f.name);
        if (name.rfind("LogiPlayLeds", 0) == 0) {
            const auto leds = reinterpret_cast<LedsFn>(reinterpret_cast<void*>(GetProcAddress(module_, name.c_str())));
            ASSERT_NE(leds, nullptr) << name;
            EXPECT_EQ(leds(0, 0.0F, 0.0F, 0.0F), 0) << name;
        } else {
            EXPECT_EQ(call(name.c_str()), 0) << name;
        }
    }
    }
    const std::string log = logText();
    EXPECT_NE(log.find("no real library at"), std::string::npos) << log;
    EXPECT_NE(log.find("no real Logitech wheel library found"), std::string::npos);
    EXPECT_NE(log.find("(not in the real library)"), std::string::npos);
}

// G HUB's newest version folder (numbered parts joined by '_', compared as numbers: 2024_10 is newer than 2024_3) wins
// over older ones and over the old Logitech software's DLL; a folder with a non-ASCII name and a plain file among the
// folders are passed over.
TEST_F(LogiDll, FindsGHubsNewestLibrary) {
    const fs::path sdk = dir_ / "programs" / "Logi" / "wheel_sdk";
    putFake(sdk / "2024_3_12" / "logi_steering_wheel_x64.dll");
    putFake(sdk / "2024_10_1" / "logi_steering_wheel_x64.dll");
    fs::create_directories(sdk / L"2025_é");
    std::ofstream(sdk / "2026_1") << "a file, not a folder";
    putFake(dir_ / "programs" / "Logitech" / "Gaming Software" / "SDKs" / "LogitechSteeringWheel.dll");
    writeIni("[logi]\nlog_level = info\n");
    load();
    EXPECT_EQ(call("LogiIsConnected", 0), 1);
    const std::string log = logText();
    EXPECT_NE(log.find("2024_10_1"), std::string::npos) << log;
    EXPECT_EQ(log.find("2024_3_12"), std::string::npos);
}

// Without G HUB, the old Logitech Gaming Software's DLL is used; %ProgramFiles% stands in when %ProgramW6432% is not
// set (a 32-bit Windows).
TEST_F(LogiDll, FallsBackToTheOldLogitechSoftware) {
    setEnvironment(L"ProgramW6432", L"");
    setEnvironment(L"ProgramFiles", (dir_ / "programs").wstring());
    putFake(dir_ / "programs" / "Logitech" / "Gaming Software" / "SDKs" / "LogitechSteeringWheel.dll");
    writeIni("[logi]\nlog_level = info\n");
    load();
    EXPECT_EQ(call("LogiIsConnected", 0), 1);
    EXPECT_NE(logText().find("LogitechSteeringWheel.dll (every call goes on to it)"), std::string::npos);
}

// The registry may point every name at this DLL: named as its own real library, it is skipped (no endless loop), and
// a candidate that exists but does not load (not a DLL) is passed over to the next one.
TEST_F(LogiDll, SkipsItselfAndWhatDoesNotLoad) {
    putFake(dir_ / "programs" / "Logitech" / "Gaming Software" / "SDKs" / "LogitechSteeringWheel.dll");
    const fs::path broken = dir_ / "programs" / "Logi" / "wheel_sdk" / "2024_1" / "logi_steering_wheel_x64.dll";
    fs::create_directories(broken.parent_path());
    std::ofstream(broken) << "not a DLL";
    writeIni("[logi]\nlog_level = info\nreal_library = " + dll().string() + "\n");
    load();
    EXPECT_EQ(call("LogiIsConnected", 0), 1);
    const std::string log = logText();
    EXPECT_NE(log.find("that is this DLL"), std::string::npos) << log;
    EXPECT_NE(log.find("loading the real library"), std::string::npos);
    EXPECT_NE(log.find("LogitechSteeringWheel.dll (every call goes on to it)"), std::string::npos);
}

// What wheelbri-logi.ini cannot use is written to the log once logging is up (other sections, other keys, a level
// with a typo, a line that is no setting), and the level falls back to the app's; FreeLibrary says goodbye in the log.
TEST_F(LogiDll, ProblemsInItsIniAreLogged) {
    writeIni("[logi]\nlog_level = loud\ncolour = red\nno equals sign here\n[other]\nx = 1\n");
    fs::create_directories(dir_ / "local" / "wheelbri");
    std::ofstream(dir_ / "local" / "wheelbri" / "app.ini") << "[app]\nlog_level = info\n";
    load();
    call("LogiSteeringShutdown");
    FreeLibrary(module_);
    module_ = nullptr;
    const std::string log = logText();
    EXPECT_NE(log.find("log_level = 'loud'"), std::string::npos) << log;
    EXPECT_NE(log.find("[logi] colour: unknown key"), std::string::npos);
    EXPECT_NE(log.find("[other]: unknown section"), std::string::npos);
    EXPECT_NE(log.find("log level info (the app's)"), std::string::npos);
    EXPECT_NE(log.find("unloaded by FreeLibrary"), std::string::npos);
}

// With no ini and no app.ini the level is warn: nothing about the calls is written, only problems.
TEST_F(LogiDll, QuietByDefault) {
    load();
    EXPECT_EQ(call("LogiIsConnected", 0), 0);
    const std::string log = logText();
    EXPECT_EQ(log.find("LogiIsConnected"), std::string::npos) << log;
    EXPECT_NE(log.find("no real Logitech wheel library found"), std::string::npos);
}

}  // namespace
