// The dinput8.dll shim's COM side, on fakes of DirectInput's objects: EmulatedDevice (the physical wheel presented as
// the target), ProxyDevice / ProxyEffect (logged and passed through), ProxyDirectInput8 (enumeration and device
// creation), and what they use: DeviceAudit, StateFeed, Guard, ShimLogging, ModuleWatch, LogFolder, RealDInput.
// Windows only (MinGW: run under Wine).

#include "ShimTestSupport.hpp"

#include "DeviceAudit.hpp"
#include "EmulatedDevice.hpp"
#include "Guard.hpp"
#include "LogFolder.hpp"
#include "ModuleWatch.hpp"
#include "ProxyDevice.hpp"
#include "ProxyDirectInput8.hpp"
#include "ProxyEffect.hpp"
#include "RealDInput.hpp"
#include "ShimConfig.hpp"
#include "ShimLogging.hpp"
#include "StateFeed.hpp"

#include "wheelbri/StatePacket.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace wheelbri::shim {
namespace {

using test::FakeDevice;
using test::FakeDirectInput;
using test::FakeEffect;

// Before any test: wheelbri-shim.ini next to the test program (the shim reads the one next to its module), with the
// tests' ports and the wheel the fakes describe (346e).
class ShimTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        std::ofstream(std::filesystem::path(gamelog::dllDirectory()) / L"wheelbri-shim.ini", std::ios::binary)
            << "[shim]\nsource_vid = 346e\ntarget_profile = g920\nstate_port = " << test::kStatePort
            << "\nffb_port = " << test::kFfbPort << "\nstale_after_ms = 1000\n";
    }
};
[[maybe_unused]] ::testing::Environment* const gEnvironment =
    ::testing::AddGlobalTestEnvironment(new ShimTestEnvironment);

// --- Guard, ShimLogging ----------------------------------------------------------------------------------------------

// Nothing escapes into the game: out of memory, any exception, anything thrown; the fallback / nothing otherwise.
TEST(ShimGuard, ExceptionsBecomeErrorCodes) {
    EXPECT_EQ(guarded("t", []() -> HRESULT { return S_FALSE; }), static_cast<HRESULT>(S_FALSE));
    EXPECT_EQ(guarded("t", []() -> HRESULT { throw std::bad_alloc(); }), static_cast<HRESULT>(E_OUTOFMEMORY));
    EXPECT_EQ(guarded("t", []() -> HRESULT { throw std::runtime_error("x"); }), static_cast<HRESULT>(DIERR_GENERIC));
    EXPECT_EQ(guarded("t", []() -> HRESULT { throw 5; }), static_cast<HRESULT>(DIERR_GENERIC));
    EXPECT_EQ(guardedOr<int>(7, "t", []() -> int { return 3; }), 3);
    EXPECT_EQ(guardedOr<int>(7, "t", []() -> int { throw std::runtime_error("x"); }), 7);
    EXPECT_EQ(guardedOr<int>(7, "t", []() -> int { throw 5; }), 7);
    int ran = 0;
    guardedVoid("t", [&] { ++ran; });
    guardedVoid("t", [] { throw std::runtime_error("x"); });
    guardedVoid("t", [] { throw 5; });
    EXPECT_EQ(ran, 1);
    noteEscapedException("t", nullptr);
}

// The service's log level: applied once per change (quieter ones after the line that says so); the same is cheap.
TEST(ShimLog, LevelChanges) {
    applyLogLevel(LogLevel::Debug);
    applyLogLevel(LogLevel::Debug);
    applyLogLevel(LogLevel::Warn);
    applyLogLevel(LogLevel::Info);
    EXPECT_NO_THROW(onDllDetach(true));   // at process exit: nothing touched
    applyLogLevel(LogLevel::Warn);
}

// --- LogFolder -------------------------------------------------------------------------------------------------------

// Sets an environment variable of this process for one test, and puts the old value back.
class ScopedEnvironment {
public:
    ScopedEnvironment(const wchar_t* name, const wchar_t* value) : name_(name) {
        wchar_t old[32768];
        const DWORD length = GetEnvironmentVariableW(name, old, 32768);
        had_ = length > 0 && length < 32768;
        if (had_) old_.assign(old, length);
        SetEnvironmentVariableW(name, value);
    }
    ~ScopedEnvironment() { SetEnvironmentVariableW(name_.c_str(), had_ ? old_.c_str() : nullptr); }

private:
    std::wstring name_;
    std::wstring old_;
    bool had_ = false;
};

// The module's own path and folder, UTF-8 of wide text, and Wine told apart.
TEST(ShimLogFolder, ModuleAndText) {
    EXPECT_FALSE(gamelog::modulePath(nullptr).empty());
    EXPECT_NE(gamelog::thisModule(), nullptr);
    const std::wstring folder = gamelog::dllDirectory();
    ASSERT_FALSE(folder.empty());
    EXPECT_TRUE(folder.back() == L'\\' || folder.back() == L'/');
    EXPECT_EQ(gamelog::utf8(L""), "");
    EXPECT_EQ(gamelog::utf8(L"é"), "\xc3\xa9");
    std::string version;
    if (gamelog::underWine(version)) {
        EXPECT_FALSE(version.empty());
    } else {
        EXPECT_TRUE(version.empty());
    }
}

// Where the log goes: under Wine the Linux user's state folder ($XDG_STATE_HOME, else ~/.local/state); on Windows (or
// Wine that cannot say) %LOCALAPPDATA%\wheelbri\logs; without that the DLL's own folder.
TEST(ShimLogFolder, LogFileFollowsTheEnvironment) {
    std::string version;
    const bool wine = gamelog::underWine(version);
    {
        const ScopedEnvironment state(L"XDG_STATE_HOME", L"/tmp/wheelbri-shimtest-state");
        const std::string path = gamelog::logFilePath(L"wheelbri-shim.log");
        EXPECT_NE(path.find("wheelbri-shim.log"), std::string::npos);
        if (wine) {
            EXPECT_NE(path.find("wheelbri-shimtest-state"), std::string::npos) << path;
        }
    }
    {
        // Not a Unix path: ~ (WINEHOME, else HOME) instead; neither: not under Wine's folders at all.
        const ScopedEnvironment state(L"XDG_STATE_HOME", L"relative");
        const ScopedEnvironment wineHome(L"WINEHOME", nullptr);
        const ScopedEnvironment home(L"HOME", L"/tmp/wheelbri-shimtest-home");
        const std::string path = gamelog::logFilePath(L"x.log");
        if (wine) {
            EXPECT_NE(path.find("wheelbri-shimtest-home"), std::string::npos) << path;
        }
    }
    {
        const ScopedEnvironment state(L"XDG_STATE_HOME", nullptr);
        const ScopedEnvironment wineHome(L"WINEHOME", nullptr);
        const ScopedEnvironment home(L"HOME", L"not-unix");
        const std::filesystem::path local = std::filesystem::temp_directory_path() / "wheelbri-shimtest-local";
        const ScopedEnvironment localAppData(L"LOCALAPPDATA", local.wstring().c_str());
        EXPECT_EQ(gamelog::logFilePath(L"x.log"), gamelog::utf8(local.wstring() + L"\\wheelbri\\logs\\x.log"));
        const ScopedEnvironment none(L"LOCALAPPDATA", nullptr);
        EXPECT_EQ(gamelog::logFilePath(L"x.log"), gamelog::utf8(gamelog::dllDirectory() + L"x.log"));
    }
    {
        // A folder with a letter outside ASCII: its 8.3 name when the volume has one, else UTF-8 as before.
        const ScopedEnvironment state(L"XDG_STATE_HOME", nullptr);
        const ScopedEnvironment wineHome(L"WINEHOME", nullptr);
        const ScopedEnvironment home(L"HOME", nullptr);
        const std::filesystem::path local = std::filesystem::temp_directory_path() / L"wheelbri-shimtest-José";
        const ScopedEnvironment localAppData(L"LOCALAPPDATA", local.wstring().c_str());
        const std::string path = gamelog::logFilePath(L"x.log");
        EXPECT_NE(path.find("x.log"), std::string::npos);
        EXPECT_TRUE(std::filesystem::exists(local / L"wheelbri" / L"logs"));   // made, to have a short name
        std::filesystem::remove_all(local);
        const ScopedEnvironment shortFolder(L"LOCALAPPDATA", L"Cé");   // too short to have one
        EXPECT_NE(gamelog::logFilePath(L"x.log").find("x.log"), std::string::npos);
    }
}

// The level the app picked: [app] log_level in its app.ini (under Wine the Linux one), warn without one.
TEST(ShimLogFolder, AppLevelAtStart) {
    std::string version;
    const bool wine = gamelog::underWine(version);
    const std::filesystem::path config = std::filesystem::temp_directory_path() / "wheelbri-shimtest-config";
    std::filesystem::create_directories(config / "wheelbri");
    std::ofstream(config / "wheelbri" / "app.ini") << "[app]\nlog_level = debug\n";
    if (wine) {
        // Wine reads the Linux folder: the test's temporary folder as Wine names it on the Unix side is not known
        // here, so the level is only checked to be one of the app's.
        const ScopedEnvironment xdg(L"XDG_CONFIG_HOME", L"/nonexistent-wheelbri-shimtest");
        EXPECT_EQ(gamelog::appLevelAtStart(), kDefaultAppLogLevel);
    }
    {
        const ScopedEnvironment xdg(L"XDG_CONFIG_HOME", nullptr);
        const ScopedEnvironment wineHome(L"WINEHOME", nullptr);
        const ScopedEnvironment home(L"HOME", nullptr);
        const ScopedEnvironment local(L"LOCALAPPDATA", config.wstring().c_str());
        EXPECT_EQ(gamelog::appLevelAtStart(), LogLevel::Debug);
        const ScopedEnvironment none(L"LOCALAPPDATA", nullptr);
        EXPECT_EQ(gamelog::appLevelAtStart(), kDefaultAppLogLevel);
    }
    std::filesystem::remove_all(config);
}

// --- ModuleWatch, RealDInput -----------------------------------------------------------------------------------------

// The first look lists every module; later ones only what was loaded since.
TEST(ShimModules, OnlyNewOnesAfterTheFirstLook) {
    logNewModules("test first");
    logNewModules("test again");   // nothing new
    const HMODULE loaded = LoadLibraryW(L"version.dll");
    logNewModules("test after LoadLibrary");
    if (loaded != nullptr) FreeLibrary(loaded);
    SUCCEED();
}

// The system's dinput8.dll is found and is not this module.
TEST(ShimRealDInput, LoadsTheSystemDll) {
    const RealDInput* const real = realDInput();
    ASSERT_NE(real, nullptr);
    EXPECT_NE(real->module, nullptr);
    EXPECT_NE(real->directInput8Create, nullptr);
    EXPECT_EQ(realDInput(), real);   // once
}

// --- DeviceAudit -----------------------------------------------------------------------------------------------------

// Calls, risky calls, properties and escapes recorded; Logitech's rev lights are not risky, other escapes are.
TEST(ShimAudit, RecordsWhatTheGameDoes) {
    DeviceAudit audit("[audit test]");
    audit.call("GetCapabilities");
    audit.call("GetCapabilities", "again");
    for (int i = 0; i < 22; ++i) audit.risky("SetActionMap", "x");   // the first 20 in full
    DIPROPDWORD dword{};
    dword.diph.dwSize = sizeof(dword);
    audit.getProperty(DIPROP_BUFFERSIZE, &dword.diph, DI_OK);
    audit.getProperty(DIPROP_BUFFERSIZE, &dword.diph, DIERR_INVALIDPARAM);
    audit.setProperty(DIPROP_RANGE, &dword.diph, DI_OK);         // scaling: not risky
    audit.setProperty(DIPROP_CALIBRATION, &dword.diph, DI_OK);   // risky
    audit.setProperty(GUID_XAxis, &dword.diph, DI_OK);           // a custom property: risky
    audit.escape(nullptr, DIERR_UNSUPPORTED);
    std::uint8_t lights[20] = {};
    lights[0] = 20;
    lights[4] = 1;
    DIEFFESCAPE escape{sizeof(DIEFFESCAPE), 0, lights, sizeof(lights), nullptr, 0};
    audit.escape(&escape, DIERR_UNSUPPORTED);
    std::uint8_t out[4] = {1, 2, 3, 4};
    escape.dwCommand = 5;
    escape.lpvOutBuffer = out;
    escape.cbOutBuffer = sizeof(out);
    audit.escape(&escape, DI_OK);
    escape.lpvInBuffer = nullptr;   // no buffers at all
    escape.cbInBuffer = 0;
    escape.lpvOutBuffer = nullptr;
    escape.cbOutBuffer = 0;
    audit.escape(&escape, DI_OK);
    audit.tick();   // starts the clock
    audit.tick();   // not due yet
    audit.summary("test");
    DeviceAudit quiet("[quiet]");
    quiet.call("GetDeviceInfo");
    quiet.summary("test");   // nothing risky: at info
    SUCCEED();
}

// --- ProxyEffect -----------------------------------------------------------------------------------------------------

// Every call goes to the real effect unchanged; its own interfaces answered itself, others by the real one; the real
// one released with it. A failed SetParameters is logged too.
TEST(ShimProxyEffect, PassesEverythingOn) {
    FakeEffect real;
    LPDIRECTINPUTEFFECT out = &real;
    wrapRealEffect(&out, GUID_Sine, "[proxy test]");
    ASSERT_NE(out, &real);
    void* object = nullptr;
    EXPECT_EQ(out->QueryInterface(IID_IUnknown, nullptr), static_cast<HRESULT>(E_POINTER));
    EXPECT_EQ(out->QueryInterface(IID_IDirectInputEffect, &object), static_cast<HRESULT>(S_OK));
    EXPECT_EQ(out->Release(), 1U);
    EXPECT_EQ(out->QueryInterface(IID_IUnknown, &object), static_cast<HRESULT>(S_OK));
    EXPECT_EQ(out->Release(), 1U);
    EXPECT_EQ(out->QueryInterface(IID_IDirectInputDevice8W, &object), static_cast<HRESULT>(E_NOINTERFACE));
    EXPECT_EQ(real.calls, 1);
    EXPECT_EQ(out->Initialize(nullptr, 0x800, GUID_Sine), static_cast<HRESULT>(DI_OK));
    GUID guid{};
    EXPECT_EQ(out->GetEffectGuid(&guid), static_cast<HRESULT>(DI_OK));
    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    EXPECT_EQ(out->GetParameters(&effect, DIEP_DURATION), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->SetParameters(&effect, DIEP_DURATION), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->Start(INFINITE, DIES_SOLO | DIES_NODOWNLOAD), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->Start(1, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->Stop(), static_cast<HRESULT>(DI_OK));
    DWORD status = 0;
    EXPECT_EQ(out->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->GetEffectStatus(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->Download(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->Unload(), static_cast<HRESULT>(DI_OK));
    std::uint8_t bytes[3] = {1, 2, 3};
    DIEFFESCAPE escape{sizeof(DIEFFESCAPE), 9, bytes, sizeof(bytes), nullptr, 0};
    EXPECT_EQ(out->Escape(&escape), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(out->Escape(nullptr), static_cast<HRESULT>(DI_OK));
    real.result = DIERR_INVALIDPARAM;
    EXPECT_EQ(out->SetParameters(&effect, DIEP_DURATION), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(out->GetParameters(&effect, DIEP_DURATION), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(out->AddRef(), 2U);
    EXPECT_EQ(out->Release(), 1U);
    EXPECT_EQ(out->Release(), 0U);
    EXPECT_EQ(real.references, 0U);
}

// --- ProxyDevice -----------------------------------------------------------------------------------------------------

// Every proxy and the emulated wheel come in DirectInput's two flavours, ANSI (A) and wide (W): each test runs on both.
using ShimTraits = ::testing::Types<WideTraits, AnsiTraits>;

// A text literal as T::String expects it (S("me"): "me" or L"me").
template <typename T>
typename T::String text(const char* ansi, const wchar_t* wide) {
    if constexpr (std::is_same_v<T, AnsiTraits>) {
        return ansi;
    } else {
        return wide;
    }
}
#define S(literal) text<TypeParam>(literal, L"" literal)

template <typename T>
class ShimProxyDevice : public ::testing::Test {};
TYPED_TEST_SUITE(ShimProxyDevice, ShimTraits);
template <typename T>
class ShimDirectInput : public ::testing::Test {};
TYPED_TEST_SUITE(ShimDirectInput, ShimTraits);

// A device that is not the wheel: every call logged and passed on; enumerations handed through to the game's
// callback; effects wrapped (not when aggregated, nor when the real call fails).
TYPED_TEST(ShimProxyDevice, PassesEverythingOn) {
    test::FakeDeviceT<TypeParam> real;
    auto* device = new ProxyDevice<TypeParam>(&real, "[other device]");
    void* object = nullptr;
    EXPECT_EQ(device->QueryInterface(IID_IUnknown, nullptr), static_cast<HRESULT>(E_POINTER));
    EXPECT_EQ(device->QueryInterface(TypeParam::deviceIid(), &object), static_cast<HRESULT>(S_OK));
    device->Release();
    EXPECT_EQ(device->QueryInterface(IID_IDirectInputEffect, &object), static_cast<HRESULT>(E_NOINTERFACE));
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    EXPECT_EQ(device->GetCapabilities(&caps), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->GetCapabilities(nullptr), static_cast<HRESULT>(DI_OK));
    int seen = 0;
    const auto onObject = [](const typename TypeParam::DeviceObjectInstance*, LPVOID ref) -> BOOL {
        ++*static_cast<int*>(ref);
        return DIENUM_CONTINUE;
    };
    EXPECT_EQ(device->EnumObjects(nullptr, nullptr, 0), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(device->EnumObjects(onObject, &seen, DIDFT_ALL), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen, 3);
    const auto stopObject = [](const typename TypeParam::DeviceObjectInstance*, LPVOID) -> BOOL { return DIENUM_STOP; };
    EXPECT_EQ(device->EnumObjects(stopObject, nullptr, DIDFT_ALL), static_cast<HRESULT>(DI_OK));
    DIPROPDWORD dword{};
    dword.diph.dwSize = sizeof(dword);
    dword.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    EXPECT_EQ(device->GetProperty(DIPROP_BUFFERSIZE, &dword.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->SetProperty(DIPROP_BUFFERSIZE, &dword.diph), static_cast<HRESULT>(DI_OK));
    typename TypeParam::DeviceObjectInstance info{};
    info.dwSize = sizeof(info);
    EXPECT_EQ(device->GetObjectInfo(&info, 0, DIPH_BYOFFSET), static_cast<HRESULT>(DI_OK));
    info.dwSize = sizeof(typename TypeParam::DeviceObjectInstanceDx3);
    EXPECT_EQ(device->GetObjectInfo(&info, 0, DIPH_BYOFFSET), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->GetObjectInfo(nullptr, 0, DIPH_BYOFFSET), static_cast<HRESULT>(DI_OK));
    typename TypeParam::DeviceInstance instance{};
    instance.dwSize = sizeof(instance);
    EXPECT_EQ(device->GetDeviceInfo(&instance), static_cast<HRESULT>(DI_OK));
    real.infoSize = sizeof(typename TypeParam::DeviceInstanceDx3);
    EXPECT_EQ(device->GetDeviceInfo(&instance), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->GetDeviceInfo(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->GetImageInfo(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->SetCooperativeLevel(nullptr, DISCL_EXCLUSIVE | DISCL_FOREGROUND), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(real.cooperativeFlags, static_cast<DWORD>(DISCL_EXCLUSIVE | DISCL_FOREGROUND));   // unchanged
    EXPECT_EQ(device->SetEventNotification(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->Initialize(nullptr, 0x800, GUID_Joystick), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->RunControlPanel(nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->BuildActionMap(nullptr, S("me"), 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->SetActionMap(nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->Acquire(), static_cast<HRESULT>(DI_OK));
    real.acquireResult = S_FALSE;   // already acquired: sampled at debug
    EXPECT_EQ(device->Acquire(), static_cast<HRESULT>(S_FALSE));
    real.acquireResult = DIERR_OTHERAPPHASPRIO;
    EXPECT_EQ(device->Acquire(), static_cast<HRESULT>(DIERR_OTHERAPPHASPRIO));
    EXPECT_EQ(device->Unacquire(), static_cast<HRESULT>(DI_OK));
    DIJOYSTATE2 state{};
    EXPECT_EQ(device->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DI_OK));
    DWORD count = 4;
    DIDEVICEOBJECTDATA data[4] = {};
    EXPECT_EQ(device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->Poll(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->SendDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->SendDeviceData(sizeof(DIDEVICEOBJECTDATA), data, nullptr, 0), static_cast<HRESULT>(DI_OK));

    LPDIRECTINPUTEFFECT effect = nullptr;
    EXPECT_EQ(device->CreateEffect(GUID_Sine, nullptr, &effect, nullptr), static_cast<HRESULT>(DI_OK));
    ASSERT_NE(effect, nullptr);
    EXPECT_NE(effect, &real.effect);   // wrapped
    effect->Release();
    real.effect.references = 1;
    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    EXPECT_EQ(device->CreateEffect(GUID_Sine, &parameters, &effect, reinterpret_cast<LPUNKNOWN>(&real)), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(effect, &real.effect);   // aggregated: not wrapped
    real.effectOut = false;
    EXPECT_EQ(device->CreateEffect(GUID_Sine, nullptr, &effect, nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->CreateEffect(GUID_Sine, nullptr, nullptr, nullptr), static_cast<HRESULT>(DI_OK));
    real.effectOut = true;

    seen = 0;
    const auto onEffect = [](const typename TypeParam::EffectInfo*, LPVOID ref) -> BOOL {
        ++*static_cast<int*>(ref);
        return DIENUM_STOP;
    };
    EXPECT_EQ(device->EnumEffects(nullptr, nullptr, DIEFT_ALL), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(device->EnumEffects(onEffect, &seen, DIEFT_ALL), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen, 2);
    EXPECT_EQ(device->GetEffectInfo(nullptr, GUID_Sine), static_cast<HRESULT>(DI_OK));
    DWORD ffState = 0;
    EXPECT_EQ(device->GetForceFeedbackState(&ffState), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->GetForceFeedbackState(nullptr), static_cast<HRESULT>(DI_OK));
    for (const DWORD command : {DWORD{DISFFC_RESET}, DWORD{DISFFC_STOPALL}, DWORD{DISFFC_PAUSE}, DWORD{DISFFC_CONTINUE},
                                DWORD{DISFFC_SETACTUATORSON}, DWORD{DISFFC_SETACTUATORSOFF}, DWORD{0x77}}) {
        EXPECT_EQ(device->SendForceFeedbackCommand(command), static_cast<HRESULT>(DI_OK));
    }
    EXPECT_EQ(device->EnumCreatedEffectObjects(nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    std::uint8_t bytes[2] = {1, 2};
    DIEFFESCAPE escape{sizeof(DIEFFESCAPE), 3, bytes, sizeof(bytes), nullptr, 0};
    EXPECT_EQ(device->Escape(&escape), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->Escape(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->EnumEffectsInFile(S("f.ffe"), nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device->WriteEffectToFile(S("f.ffe"), 0, nullptr, 0), static_cast<HRESULT>(DI_OK));

    // Failures pass through as they are.
    real.result = DIERR_INPUTLOST;
    EXPECT_EQ(device->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DIERR_INPUTLOST));
    EXPECT_EQ(device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DIERR_INPUTLOST));
    EXPECT_EQ(device->Poll(), static_cast<HRESULT>(DIERR_INPUTLOST));
    EXPECT_EQ(device->GetCapabilities(&caps), static_cast<HRESULT>(DIERR_INPUTLOST));
    EXPECT_EQ(device->GetObjectInfo(&info, 0, DIPH_BYOFFSET), static_cast<HRESULT>(DIERR_INPUTLOST));
    EXPECT_EQ(device->CreateEffect(GUID_Sine, nullptr, &effect, nullptr), static_cast<HRESULT>(DIERR_INPUTLOST));
    EXPECT_EQ(device->GetProperty(DIPROP_BUFFERSIZE, &dword.diph), static_cast<HRESULT>(DIERR_INPUTLOST));

    EXPECT_EQ(device->Release(), 0U);
    EXPECT_EQ(real.references, 0U);
}

// --- EmulatedDevice --------------------------------------------------------------------------------------------------

// The physical wheel wrapped as the G920, released after each test (its effects stopped, the real device released).
template <typename T>
class ShimEmulated : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = new EmulatedDevice<T>(&real_, makeProfile("g920"), "Moza R3");
    }
    void TearDown() override {
        if (device_ != nullptr) {
            EXPECT_EQ(device_->Release(), 0U);
            EXPECT_EQ(real_.references, 0U);
        }
    }

    // The game's c_dfDIJoystick2, acquired.
    void ready() {
        ASSERT_EQ(device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
        ASSERT_EQ(device_->Acquire(), static_cast<HRESULT>(DI_OK));
    }

    static DIPROPDWORD dword(const DWORD how, const DWORD object, const DWORD value = 0) {
        DIPROPDWORD p{};
        p.diph.dwSize = sizeof(p);
        p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        p.diph.dwHow = how;
        p.diph.dwObj = object;
        p.dwData = value;
        return p;
    }
    static DIPROPRANGE range(const DWORD how, const DWORD object, const LONG min = 0, const LONG max = 0) {
        DIPROPRANGE p{};
        p.diph.dwSize = sizeof(p);
        p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        p.diph.dwHow = how;
        p.diph.dwObj = object;
        p.lMin = min;
        p.lMax = max;
        return p;
    }

    test::FakeDeviceT<T> real_;
    EmulatedDevice<T>* device_ = nullptr;
};
TYPED_TEST_SUITE(ShimEmulated, ShimTraits);

// It is the target to the game: its own interfaces, the G920's capabilities and identity over the real ones.
TYPED_TEST(ShimEmulated, PresentsTheTarget) {
    void* object = nullptr;
    EXPECT_EQ(this->device_->QueryInterface(IID_IUnknown, nullptr), static_cast<HRESULT>(E_POINTER));
    EXPECT_EQ(this->device_->QueryInterface(TypeParam::deviceIid(), &object), static_cast<HRESULT>(S_OK));
    EXPECT_EQ(this->device_->Release(), 1U);
    EXPECT_EQ(this->device_->QueryInterface(IID_IDirectInputEffect, &object), static_cast<HRESULT>(E_NOINTERFACE));

    EXPECT_EQ(this->device_->GetCapabilities(nullptr), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    DIDEVCAPS caps{};
    caps.dwSize = 4;
    EXPECT_EQ(this->device_->GetCapabilities(&caps), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    caps.dwSize = sizeof(caps);
    ASSERT_EQ(this->device_->GetCapabilities(&caps), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(GET_DIDEVICE_TYPE(caps.dwDevType), static_cast<DWORD>(DI8DEVTYPE_DRIVING));
    EXPECT_NE(caps.dwFlags & DIDC_FORCEFEEDBACK, 0U);   // force feedback through the service
    EXPECT_GT(caps.dwButtons, 10U);
    this->real_.result = DIERR_INPUTLOST;
    EXPECT_EQ(this->device_->GetCapabilities(&caps), static_cast<HRESULT>(DIERR_INPUTLOST));
    this->real_.result = DI_OK;

    typename TypeParam::DeviceInstance info{};
    info.dwSize = sizeof(info);
    ASSERT_EQ(this->device_->GetDeviceInfo(&info), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(toUtf8(info.tszProductName), makeProfile("g920")->identity().productName);
    EXPECT_EQ(this->device_->GetDeviceInfo(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetImageInfo(nullptr), static_cast<HRESULT>(DIERR_UNSUPPORTED));
}

// Its objects: filtered by kind and force feedback actuators, never trigger buttons; the game may stop early.
TYPED_TEST(ShimEmulated, EnumeratesItsObjects) {
    EXPECT_EQ(this->device_->EnumObjects(nullptr, nullptr, DIDFT_ALL), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    struct Seen {
        int all = 0;
        int axes = 0;
    } seen;
    const auto count = [](const typename TypeParam::DeviceObjectInstance* object, LPVOID ref) -> BOOL {
        auto* s = static_cast<Seen*>(ref);
        ++s->all;
        if ((object->dwType & DIDFT_AXIS) != 0) ++s->axes;
        return DIENUM_CONTINUE;
    };
    ASSERT_EQ(this->device_->EnumObjects(count, &seen, DIDFT_ALL), static_cast<HRESULT>(DI_OK));
    const int all = seen.all;
    EXPECT_GT(all, 10);
    seen = {};
    ASSERT_EQ(this->device_->EnumObjects(count, &seen, DIDFT_AXIS), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen.all, seen.axes);
    seen = {};
    ASSERT_EQ(this->device_->EnumObjects(count, &seen, DIDFT_AXIS | DIDFT_FFACTUATOR), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen.all, 1);   // the wheel
    seen = {};
    ASSERT_EQ(this->device_->EnumObjects(count, &seen, DIDFT_BUTTON | DIDFT_FFEFFECTTRIGGER), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen.all, 0);
    seen = {};
    ASSERT_EQ(this->device_->EnumObjects(count, &seen, DIDFT_POV), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen.all, 1);
    const auto stop = [](const typename TypeParam::DeviceObjectInstance*, LPVOID ref) -> BOOL {
        ++*static_cast<int*>(ref);
        return DIENUM_STOP;
    };
    int calls = 0;
    ASSERT_EQ(this->device_->EnumObjects(stop, &calls, DIDFT_ALL), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(calls, 1);
}

// GetObjectInfo: only DirectInput's two sizes; by offset, id or usage; unknown objects are not found.
TYPED_TEST(ShimEmulated, ObjectInfo) {
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetObjectInfo(nullptr, 0, DIPH_BYOFFSET), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    typename TypeParam::DeviceObjectInstance info{};
    info.dwSize = 10;
    EXPECT_EQ(this->device_->GetObjectInfo(&info, 0, DIPH_BYOFFSET), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    info.dwSize = sizeof(info);
    ASSERT_EQ(this->device_->GetObjectInfo(&info, DIJOFS_X, DIPH_BYOFFSET), static_cast<HRESULT>(DI_OK));
    EXPECT_TRUE(info.guidType == GUID_XAxis);
    EXPECT_NE(info.dwFlags & DIDOI_FFACTUATOR, 0U);
    const DWORD wheelId = info.dwType;
    const WORD page = info.wUsagePage;
    const WORD usage = info.wUsage;
    info.dwSize = sizeof(typename TypeParam::DeviceObjectInstanceDx3);
    ASSERT_EQ(this->device_->GetObjectInfo(&info, wheelId, DIPH_BYID), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(info.dwSize, sizeof(typename TypeParam::DeviceObjectInstanceDx3));
    info.dwSize = sizeof(info);
    EXPECT_EQ(this->device_->GetObjectInfo(&info, MAKELONG(usage, page), DIPH_BYUSAGE), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetObjectInfo(&info, 9999, DIPH_BYOFFSET), static_cast<HRESULT>(DIERR_OBJECTNOTFOUND));
    EXPECT_EQ(this->device_->GetObjectInfo(&info, 0, 77), static_cast<HRESULT>(DIERR_OBJECTNOTFOUND));   // no such way to name one
}

// Properties it answers itself: identity, ranges, dead zone and saturation as set, buffer size, gain, centring; the
// wheel's own device-level ones from the real device; objects asked where only the device makes sense and back.
TYPED_TEST(ShimEmulated, Properties) {
    EXPECT_EQ(this->device_->GetProperty(DIPROP_RANGE, nullptr), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    DIPROPHEADER bad{};
    bad.dwHeaderSize = 3;
    EXPECT_EQ(this->device_->GetProperty(DIPROP_RANGE, &bad), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, nullptr), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, &bad), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));

    DIPROPDWORD vidPid = TestFixture::dword(DIPH_DEVICE, 0);
    ASSERT_EQ(this->device_->GetProperty(DIPROP_VIDPID, &vidPid.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(LOWORD(vidPid.dwData), makeProfile("g920")->identity().vendorId);
    DIPROPSTRING text{};
    text.diph.dwSize = sizeof(text);
    text.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    text.diph.dwHow = DIPH_DEVICE;
    EXPECT_EQ(this->device_->GetProperty(DIPROP_PRODUCTNAME, &text.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_INSTANCENAME, &text.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_TYPENAME, &text.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(toUtf8(text.wsz).substr(0, 4), "VID_");
    EXPECT_EQ(this->device_->GetProperty(DIPROP_TYPENAME, &vidPid.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));   // wrong size for text
    EXPECT_EQ(this->device_->GetProperty(DIPROP_VIDPID, &text.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));       // wrong size for a dword
    DIPROPDWORD value = TestFixture::dword(DIPH_DEVICE, 0);
    EXPECT_EQ(this->device_->GetProperty(DIPROP_AXISMODE, &value.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(value.dwData, static_cast<DWORD>(DIPROPAXISMODE_ABS));

    // Ranges: per axis; device-wide for every axis; a button or nothing at all has none.
    DIPROPRANGE r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_X, -100, 100);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DI_OK));
    r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_X);
    ASSERT_EQ(this->device_->GetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(r.lMin, -100);
    EXPECT_EQ(r.lMax, 100);
    r = TestFixture::range(DIPH_DEVICE, 0, 0, 1000);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DI_OK));
    r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_X, 5, 5);   // empty range
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_BUTTON0, 0, 10);   // a button: no axis to range
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DIERR_OBJECTNOTFOUND));
    r = TestFixture::range(DIPH_DEVICE, 0);
    EXPECT_EQ(this->device_->GetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    DIPROPDWORD wrongSize = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_X);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, &wrongSize.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_RANGE, &wrongSize.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    r = TestFixture::range(DIPH_BYOFFSET, 9999, 0, 10);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_RANGE, &r.diph), static_cast<HRESULT>(DIERR_OBJECTNOTFOUND));

    // Logical and physical ranges: an axis, a button, the POV; none for the device.
    for (const GUID* property : {&DIPROP_LOGICALRANGE, &DIPROP_PHYSICALRANGE}) {
        r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_X);
        ASSERT_EQ(this->device_->GetProperty(*property, &r.diph), static_cast<HRESULT>(DI_OK));
        EXPECT_EQ(r.lMax, 65535);
        r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_BUTTON0);
        ASSERT_EQ(this->device_->GetProperty(*property, &r.diph), static_cast<HRESULT>(DI_OK));
        EXPECT_EQ(r.lMax, 1);
        r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_POV(0));
        ASSERT_EQ(this->device_->GetProperty(*property, &r.diph), static_cast<HRESULT>(DI_OK));
        EXPECT_EQ(r.lMax, 35900);
        r = TestFixture::range(DIPH_DEVICE, 0);
        EXPECT_EQ(this->device_->GetProperty(*property, &r.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    }

    // Dead zone and saturation: kept per axis (not applied), at most 10000, axes only.
    DIPROPDWORD zone = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_Y, 500);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_DEADZONE, &zone.diph), static_cast<HRESULT>(DI_OK));
    zone.dwData = 0;
    ASSERT_EQ(this->device_->GetProperty(DIPROP_DEADZONE, &zone.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(zone.dwData, 500U);
    zone = TestFixture::dword(DIPH_DEVICE, 0, 9000);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_SATURATION, &zone.diph), static_cast<HRESULT>(DI_OK));
    zone = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_RZ);
    ASSERT_EQ(this->device_->GetProperty(DIPROP_SATURATION, &zone.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(zone.dwData, 9000U);
    zone = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_X, 10001);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_DEADZONE, &zone.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    zone = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_BUTTON0, 10);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_DEADZONE, &zone.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_DEADZONE, &zone.diph), static_cast<HRESULT>(DIERR_OBJECTNOTFOUND));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_SATURATION, &zone.diph), static_cast<HRESULT>(DIERR_OBJECTNOTFOUND));
    r = TestFixture::range(DIPH_BYOFFSET, DIJOFS_X);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_SATURATION, &r.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));   // wrong size

    // Granularity: 1 for any object; none for the device.
    zone = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_BUTTON0);
    ASSERT_EQ(this->device_->GetProperty(DIPROP_GRANULARITY, &zone.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(zone.dwData, 1U);
    zone = TestFixture::dword(DIPH_DEVICE, 0);
    EXPECT_EQ(this->device_->GetProperty(DIPROP_GRANULARITY, &zone.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));

    // Buffer size: device only, capped, not while acquired.
    DIPROPDWORD buffer = TestFixture::dword(DIPH_DEVICE, 0, 1'000'000);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph), static_cast<HRESULT>(DI_OK));
    buffer.dwData = 0;
    ASSERT_EQ(this->device_->GetProperty(DIPROP_BUFFERSIZE, &buffer.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(buffer.dwData, 4096U);
    buffer = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_X, 8);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    r = TestFixture::range(DIPH_DEVICE, 0);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_BUFFERSIZE, &r.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));

    // Axis mode and calibration mode: accepted, nothing to do.
    zone = TestFixture::dword(DIPH_DEVICE, 0, DIPROPAXISMODE_ABS);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_AXISMODE, &zone.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->SetProperty(DIPROP_CALIBRATIONMODE, &zone.diph), static_cast<HRESULT>(DI_OK));

    // Force feedback gain (service mode): kept by the forwarder; centring: off and stays off.
    DIPROPDWORD gain = TestFixture::dword(DIPH_DEVICE, 0, 20000);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_FFGAIN, &gain.diph), static_cast<HRESULT>(DI_OK));
    gain.dwData = 0;
    ASSERT_EQ(this->device_->GetProperty(DIPROP_FFGAIN, &gain.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(gain.dwData, 10000U);
    gain = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_X, 5000);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_FFGAIN, &gain.diph), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    DIPROPDWORD centre = TestFixture::dword(DIPH_DEVICE, 0, DIPROPAUTOCENTER_ON);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_AUTOCENTER, &centre.diph), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(this->device_->GetProperty(DIPROP_AUTOCENTER, &centre.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(centre.dwData, static_cast<DWORD>(DIPROPAUTOCENTER_OFF));

    // The HID path and the device's other properties: the physical wheel's; an object's: not supported.
    DIPROPGUIDANDPATH path{};
    path.diph.dwSize = sizeof(path);
    path.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    path.diph.dwHow = DIPH_DEVICE;
    const std::size_t asked = this->real_.properties.size();
    EXPECT_EQ(this->device_->GetProperty(DIPROP_GUIDANDPATH, &path.diph), static_cast<HRESULT>(DI_OK));
    DIPROPDWORD id = TestFixture::dword(DIPH_DEVICE, 0);
    EXPECT_EQ(this->device_->GetProperty(DIPROP_JOYSTICKID, &id.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(id.dwData, 1234U);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_JOYSTICKID, &id.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->real_.properties.size(), asked + 3);
    id = TestFixture::dword(DIPH_BYOFFSET, DIJOFS_X);
    EXPECT_EQ(this->device_->GetProperty(DIPROP_JOYSTICKID, &id.diph), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(this->device_->SetProperty(DIPROP_JOYSTICKID, &id.diph), static_cast<HRESULT>(DIERR_UNSUPPORTED));
}

// Its data format: checked as DirectInput does, refused while acquired; the physical wheel gets it too, and
// c_dfDIJoystick when it refuses the game's.
TYPED_TEST(ShimEmulated, DataFormat) {
    EXPECT_EQ(this->device_->SetDataFormat(nullptr), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    DIDATAFORMAT format = c_dfDIJoystick2;
    format.dwSize = 4;
    EXPECT_EQ(this->device_->SetDataFormat(&format), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    format = c_dfDIJoystick2;
    format.dwObjSize = 4;
    EXPECT_EQ(this->device_->SetDataFormat(&format), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    format = c_dfDIJoystick2;
    format.rgodf = nullptr;
    EXPECT_EQ(this->device_->SetDataFormat(&format), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    format = c_dfDIJoystick2;
    format.dwDataSize = 2;   // the objects lie outside it
    EXPECT_EQ(this->device_->SetDataFormat(&format), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    DIDATAFORMAT empty{sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, 4, 0, nullptr};
    EXPECT_EQ(this->device_->SetDataFormat(&empty), static_cast<HRESULT>(DI_OK));
    const int before = this->real_.dataFormats;
    this->real_.dataFormatResult = DIERR_INVALIDPARAM;
    EXPECT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    // The game's, then c_dfDIJoystick from the real DLL (Wine's dinput8 does not export GetdfDIJoystick: none then).
    const RealDInput* const realDll = realDInput();
    const bool standard = realDll != nullptr && realDll->getdfDIJoystick != nullptr;
    EXPECT_EQ(this->real_.dataFormats, before + (standard ? 2 : 1));
    this->real_.dataFormatResult = DI_OK;
    ASSERT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DIERR_ACQUIRED));
}

// Device management goes to the physical wheel, opened non-exclusively in service mode; Acquire never fails for the
// physical wheel's sake; Poll and Unacquire answer as an acquired / unacquired device does.
TYPED_TEST(ShimEmulated, AcquireAndCooperate) {
    EXPECT_EQ(this->device_->SetCooperativeLevel(nullptr, DISCL_EXCLUSIVE | DISCL_FOREGROUND), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->real_.cooperativeFlags, static_cast<DWORD>(DISCL_NONEXCLUSIVE | DISCL_BACKGROUND));
    EXPECT_EQ(this->device_->SetEventNotification(nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->Initialize(nullptr, 0x800, GUID_Joystick), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->RunControlPanel(nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->BuildActionMap(nullptr, nullptr, 0), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(this->device_->SetActionMap(nullptr, nullptr, 0), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(this->device_->Poll(), static_cast<HRESULT>(DIERR_NOTACQUIRED));
    this->real_.acquireResult = DIERR_OTHERAPPHASPRIO;
    EXPECT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));   // ignored: the service has the wheel
    EXPECT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));   // already acquired
    EXPECT_EQ(this->device_->Poll(), static_cast<HRESULT>(DI_NOEFFECT));
    EXPECT_EQ(this->device_->Unacquire(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->Unacquire(), static_cast<HRESULT>(DI_OK));   // already not
    std::uint32_t sent = 0;
    EXPECT_EQ(this->device_->SendDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    DWORD count = 1;
    EXPECT_EQ(this->device_->SendDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &count, 0), static_cast<HRESULT>(DI_OK));
    (void)sent;
}

// Reading: the state in the game's format, only for its size and while acquired; with no state from the service a
// centred wheel and released pedals.
TYPED_TEST(ShimEmulated, ReadsTheState) {
    DIJOYSTATE2 state{};
    EXPECT_EQ(this->device_->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DIERR_INVALIDPARAM));   // no format yet
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DIERR_NOTACQUIRED));
    EXPECT_EQ(this->device_->GetDeviceState(sizeof(state), nullptr), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->GetDeviceState(sizeof(DIJOYSTATE), &state), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    ASSERT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));
    for (int i = 0; i < 12; ++i) {   // the first reads are logged in full, then sampled
        ASSERT_EQ(this->device_->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DI_OK));
    }
    EXPECT_NEAR(state.lX, 32767, 2);
    EXPECT_EQ(state.rgdwPOV[0], 0xFFFFFFFFUL);
}

// Buffered data: refused without a buffer and before Acquire; a flush and a peek with no data, and what DirectInput
// says for each.
TYPED_TEST(ShimEmulated, BufferedDataRules) {
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    DWORD count = 4;
    DIDEVICEOBJECTDATA data[4] = {};
    EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, nullptr, 0), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->GetDeviceData(7, data, &count, 0), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DIERR_NOTBUFFERED));
    DIPROPDWORD buffer = TestFixture::dword(DIPH_DEVICE, 0, 2);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DIERR_NOTACQUIRED));
    ASSERT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph), static_cast<HRESULT>(DIERR_ACQUIRED));
    count = 4;
    EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA_DX3), data, &count, DIGDD_PEEK), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(count, 0U);
    count = INFINITE;
    EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &count, 0), static_cast<HRESULT>(DI_OK));   // flush
    EXPECT_EQ(count, 0U);
    count = INFINITE;
    EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &count, DIGDD_PEEK), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(count, 0U);
}

// The state from wheelbri-service: what the game reads, and as buffered changes (the oldest dropped when the buffer is
// full, which the next read says). Only when the service is let through: a sender Windows names and that is not
// WheelBri (this test program, on Windows) is refused, and the wheel stays centred.
TYPED_TEST(ShimEmulated, StateFromTheService) {
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    DIPROPDWORD buffer = TestFixture::dword(DIPH_DEVICE, 0, 2);
    ASSERT_EQ(this->device_->SetProperty(DIPROP_BUFFERSIZE, &buffer.diph), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));
    const auto profile = makeProfile("g920");
    auto publisher = UdpLoopbackPublisher::create(test::kStatePort);
    ASSERT_NE(publisher, nullptr);
    WheelState wheel;
    wheel.steering = 1.0F;
    wheel.throttle = 1.0F;
    wheel.buttons = 0x7;
    StatePacket packet;
    packet.sequence = 1;
    packet.report = profile->encode(wheel);
    ASSERT_TRUE(publisher->publish(packet));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    DIJOYSTATE2 state{};
    ASSERT_EQ(this->device_->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DI_OK));
    if (state.lX > 60000) {
        // Let through: the buffer holds the newest changes, the oldest gone.
        DWORD count = 8;
        DIDEVICEOBJECTDATA data[8] = {};
        EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, DIGDD_PEEK), static_cast<HRESULT>(DI_BUFFEROVERFLOW));
        EXPECT_EQ(count, 2U);
        EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DI_BUFFEROVERFLOW));
        count = 8;
        EXPECT_EQ(this->device_->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), data, &count, 0), static_cast<HRESULT>(DI_OK));
        EXPECT_EQ(count, 0U);
        // Nothing more for longer than stale_after_ms: centred again.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        ASSERT_EQ(this->device_->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DI_OK));
        EXPECT_NEAR(state.lX, 32767, 2);
    } else {
        EXPECT_NEAR(state.lX, 32767, 2);
    }
}

// Force feedback through the service: effects of the supported types in slots of their own; refused when aggregated,
// unsupported, full, or with parameters that cannot be read (the slot given back); the device's commands and state.
TYPED_TEST(ShimEmulated, EffectsThroughTheService) {
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    LPDIRECTINPUTEFFECT effect = nullptr;
    EXPECT_EQ(this->device_->CreateEffect(GUID_ConstantForce, nullptr, nullptr, nullptr), static_cast<HRESULT>(E_POINTER));
    EXPECT_EQ(this->device_->CreateEffect(GUID_ConstantForce, nullptr, &effect, reinterpret_cast<LPUNKNOWN>(&this->real_)),
              CLASS_E_NOAGGREGATION);
    EXPECT_EQ(this->device_->CreateEffect(GUID_CustomForce, nullptr, &effect, nullptr), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(effect, nullptr);
    DIEFFECT bad{};
    bad.dwSize = 3;
    EXPECT_EQ(this->device_->CreateEffect(GUID_ConstantForce, &bad, &effect, nullptr), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(effect, nullptr);

    std::vector<LPDIRECTINPUTEFFECT> effects;
    DICONSTANTFORCE force{5000};
    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    parameters.dwDuration = INFINITE;
    parameters.dwGain = 10000;
    parameters.cbTypeSpecificParams = sizeof(force);
    parameters.lpvTypeSpecificParams = &force;
    for (std::size_t i = 0; i < IFfbSink::kMaxSlots; ++i) {
        LPDIRECTINPUTEFFECT made = nullptr;
        ASSERT_EQ(this->device_->CreateEffect(GUID_ConstantForce, i == 0 ? &parameters : nullptr, &made, nullptr), static_cast<HRESULT>(DI_OK));
        effects.push_back(made);
    }
    parameters.dwSize = sizeof(DIEFFECT_DX5);
    EXPECT_EQ(this->device_->CreateEffect(GUID_ConstantForce, &parameters, &effect, nullptr), static_cast<HRESULT>(DIERR_DEVICEFULL));
    effects.back()->Release();
    effects.pop_back();
    ASSERT_EQ(this->device_->CreateEffect(GUID_ConstantForce, &parameters, &effect, nullptr), static_cast<HRESULT>(DI_OK));   // a DirectX 5 one
    effects.push_back(effect);
    EXPECT_EQ(effects.front()->Start(1, 0), static_cast<HRESULT>(DI_OK));

    DWORD state = 0;
    EXPECT_EQ(this->device_->GetForceFeedbackState(nullptr), static_cast<HRESULT>(E_POINTER));
    ASSERT_EQ(this->device_->GetForceFeedbackState(&state), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(state & DIGFFS_STOPPED, 0U);
    for (const DWORD command : {DISFFC_PAUSE, DISFFC_CONTINUE, DISFFC_SETACTUATORSOFF, DISFFC_SETACTUATORSON,
                                DISFFC_STOPALL, DISFFC_RESET}) {
        EXPECT_EQ(this->device_->SendForceFeedbackCommand(command), static_cast<HRESULT>(DI_OK));
    }
    EXPECT_EQ(this->device_->SendForceFeedbackCommand(0x77), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(this->device_->EnumCreatedEffectObjects(nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->EnumEffects(nullptr, nullptr, DIEFT_ALL), static_cast<HRESULT>(DI_OK));   // the physical wheel's list
    EXPECT_EQ(this->device_->GetEffectInfo(nullptr, GUID_Sine), static_cast<HRESULT>(DI_OK));
    for (LPDIRECTINPUTEFFECT e : effects) e->Release();

    std::uint8_t bytes[40] = {};
    DIEFFESCAPE escape{sizeof(DIEFFESCAPE), 3, bytes, sizeof(bytes), nullptr, 0};
    for (int i = 0; i < 7; ++i) EXPECT_EQ(this->device_->Escape(&escape), static_cast<HRESULT>(DIERR_UNSUPPORTED));   // sampled after 5
    EXPECT_EQ(this->device_->Escape(nullptr), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    escape.lpvInBuffer = nullptr;
    EXPECT_EQ(this->device_->Escape(&escape), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(this->device_->EnumEffectsInFile(S("f"), nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->WriteEffectToFile(S("f"), 0, nullptr, 0), static_cast<HRESULT>(DI_OK));
}

// The force feedback mode for one test (force_feedback in wheelbri-shim.ini, read once per process: the test changes
// the loaded settings and puts them back).
class FfbModeScope {
public:
    explicit FfbModeScope(const FfbMode mode) : before_(shimConfig().forceFeedback) {
        const_cast<ShimConfig&>(shimConfig()).forceFeedback = mode;
    }
    ~FfbModeScope() { const_cast<ShimConfig&>(shimConfig()).forceFeedback = before_; }
    FfbModeScope(const FfbModeScope&) = delete;
    FfbModeScope& operator=(const FfbModeScope&) = delete;

private:
    FfbMode before_;
};

// force_feedback = passthrough: the physical wheel plays the game's effects itself (opened as the game asks, its own
// effects wrapped and logged, its errors passed on), and its force feedback properties and state are its own.
TYPED_TEST(ShimEmulated, PassthroughLeavesForceFeedbackToTheWheel) {
    const FfbModeScope mode(FfbMode::Passthrough);
    EXPECT_EQ(this->device_->SetCooperativeLevel(nullptr, DISCL_EXCLUSIVE | DISCL_FOREGROUND), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->real_.cooperativeFlags, static_cast<DWORD>(DISCL_EXCLUSIVE | DISCL_FOREGROUND));
    this->real_.acquireResult = DIERR_OTHERAPPHASPRIO;   // not ignored here
    EXPECT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DIERR_OTHERAPPHASPRIO));
    this->real_.acquireResult = DI_OK;
    ASSERT_EQ(this->device_->SetDataFormat(&c_dfDIJoystick2), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(this->device_->Acquire(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->Poll(), static_cast<HRESULT>(DI_OK));   // the wheel's own answer
    DIJOYSTATE2 state{};
    EXPECT_EQ(this->device_->GetDeviceState(sizeof(state), &state), static_cast<HRESULT>(DI_OK));

    LPDIRECTINPUTEFFECT effect = nullptr;
    ASSERT_EQ(this->device_->CreateEffect(GUID_Sine, nullptr, &effect, nullptr), static_cast<HRESULT>(DI_OK));
    ASSERT_NE(effect, nullptr);
    EXPECT_NE(effect, &this->real_.effect);   // the wheel's own effect, wrapped
    effect->Release();
    this->real_.effect.references = 1;
    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    EXPECT_EQ(this->device_->CreateEffect(GUID_Sine, &parameters, &effect, reinterpret_cast<LPUNKNOWN>(&this->real_)),
              static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(effect, &this->real_.effect);   // aggregated: as it is
    this->real_.result = DIERR_DEVICEFULL;
    EXPECT_EQ(this->device_->CreateEffect(GUID_Sine, nullptr, &effect, nullptr), static_cast<HRESULT>(DIERR_DEVICEFULL));
    EXPECT_EQ(this->device_->SendForceFeedbackCommand(DISFFC_RESET), static_cast<HRESULT>(DIERR_DEVICEFULL));
    this->real_.result = DI_OK;

    DWORD ffState = 0;
    EXPECT_EQ(this->device_->GetForceFeedbackState(&ffState), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(ffState, static_cast<DWORD>(DIGFFS_STOPPED));   // the fake wheel's
    EXPECT_EQ(this->device_->SendForceFeedbackCommand(DISFFC_STOPALL), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->EnumCreatedEffectObjects(nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    int effects = 0;
    const auto count = [](const typename TypeParam::EffectInfo*, LPVOID ref) -> BOOL {
        ++*static_cast<int*>(ref);
        return DIENUM_CONTINUE;
    };
    EXPECT_EQ(this->device_->EnumEffects(count, &effects, DIEFT_ALL), static_cast<HRESULT>(DI_OK));
    EXPECT_GT(effects, 0);

    auto gain = TestFixture::dword(DIPH_DEVICE, 0, 7000);
    const std::size_t asked = this->real_.properties.size();
    EXPECT_EQ(this->device_->SetProperty(DIPROP_FFGAIN, &gain.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_FFGAIN, &gain.diph), static_cast<HRESULT>(DI_OK));
    auto centre = TestFixture::dword(DIPH_DEVICE, 0, DIPROPAUTOCENTER_ON);
    EXPECT_EQ(this->device_->SetProperty(DIPROP_AUTOCENTER, &centre.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->device_->GetProperty(DIPROP_AUTOCENTER, &centre.diph), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(this->real_.properties.size(), asked + 4);   // all four went to the wheel
    EXPECT_EQ(this->device_->Unacquire(), static_cast<HRESULT>(DI_OK));
}

// force_feedback = off: the game is told the wheel has no force feedback, gets no effects and lists none.
TYPED_TEST(ShimEmulated, OffHidesForceFeedback) {
    const FfbModeScope mode(FfbMode::Off);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    ASSERT_EQ(this->device_->GetCapabilities(&caps), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(caps.dwFlags & DIDC_FORCEFEEDBACK, 0U);
    LPDIRECTINPUTEFFECT effect = nullptr;
    EXPECT_EQ(this->device_->CreateEffect(GUID_ConstantForce, nullptr, &effect, nullptr), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(effect, nullptr);
    int effects = 0;
    const auto count = [](const typename TypeParam::EffectInfo*, LPVOID ref) -> BOOL {
        ++*static_cast<int*>(ref);
        return DIENUM_CONTINUE;
    };
    EXPECT_EQ(this->device_->EnumEffects(count, &effects, DIEFT_ALL), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(effects, 0);
}

// Released while an effect still plays: the effect stops (the forwarder's stopAll for this device).
TYPED_TEST(ShimEmulated, ReleaseStopsItsEffects) {
    LPDIRECTINPUTEFFECT effect = nullptr;
    ASSERT_EQ(this->device_->CreateEffect(GUID_Sine, nullptr, &effect, nullptr), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(effect->Start(INFINITE, 0), static_cast<HRESULT>(DI_OK));
    DWORD status = 0;
    ASSERT_EQ(effect->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(status, static_cast<DWORD>(DIEGES_PLAYING));
    EXPECT_EQ(this->device_->Release(), 0U);
    this->device_ = nullptr;
    ASSERT_EQ(effect->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(status, 0U);
    effect->Release();
}

// --- ProxyDirectInput8 -----------------------------------------------------------------------------------------------

// Enumeration: the physical wheel offered as the target (also to a game asking only for driving wheels, which the
// real DLL is asked for all game controllers instead); other devices as they are; the game may stop at any device.
TYPED_TEST(ShimDirectInput, EnumeratesThePhysicalWheelAsTheTarget) {
    test::FakeDirectInputT<TypeParam> real;
    auto* dinput = new ProxyDirectInput8<TypeParam>(&real);
    real.instances.push_back(test::instanceOf<TypeParam>(test::productGuid(0x046D, 0xC52B), "Mouse", DI8DEVTYPE_MOUSE));
    real.instances.push_back(test::instanceOf<TypeParam>(test::productGuid(0x346E, 0x0005), "Moza R3",
                                              DI8DEVTYPE_JOYSTICK | DIDEVTYPE_HID));
    real.instances.push_back(test::instanceOf<TypeParam>(test::productGuid(0x1234, 0x0001), "Old pad", DI8DEVTYPE_GAMEPAD,
                                              sizeof(typename TypeParam::DeviceInstanceDx3)));
    struct Seen {
        std::vector<std::string> names;
        bool stopAtWheel = false;
    } seen;
    const auto onDevice = [](const typename TypeParam::DeviceInstance* instance, LPVOID ref) -> BOOL {
        auto* s = static_cast<Seen*>(ref);
        s->names.push_back(toUtf8(instance->tszProductName));
        return s->stopAtWheel && GET_DIDEVICE_TYPE(instance->dwDevType) == DI8DEVTYPE_DRIVING ? DIENUM_STOP
                                                                                               : DIENUM_CONTINUE;
    };
    EXPECT_EQ(dinput->EnumDevices(DI8DEVCLASS_ALL, nullptr, nullptr, 0), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    ASSERT_EQ(dinput->EnumDevices(DI8DEVCLASS_ALL, onDevice, &seen, DIEDFL_ATTACHEDONLY), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(seen.names.size(), 3U);
    EXPECT_EQ(seen.names[1], makeProfile("g920")->identity().productName);
    EXPECT_EQ(real.lastFilter, static_cast<DWORD>(DI8DEVCLASS_ALL));

    seen = {};
    ASSERT_EQ(dinput->EnumDevices(DI8DEVTYPE_DRIVING, onDevice, &seen, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(real.lastFilter, static_cast<DWORD>(DI8DEVCLASS_GAMECTRL));
    ASSERT_EQ(seen.names.size(), 1U);   // only the wheel is a driving device
    seen = {};
    seen.stopAtWheel = true;
    ASSERT_EQ(dinput->EnumDevices(DI8DEVCLASS_ALL, onDevice, &seen, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(seen.names.size(), 2U);   // stopped there
    EXPECT_EQ(dinput->Release(), 0U);
    EXPECT_EQ(real.references, 0U);
}

// CreateDevice: the physical wheel becomes the target, anything else a logged proxy; aggregated or failed calls are
// passed through; a device the real DLL cannot describe is a proxy too.
TYPED_TEST(ShimDirectInput, CreatesTheRightWrapper) {
    test::FakeDirectInputT<TypeParam> real;
    auto* dinput = new ProxyDirectInput8<TypeParam>(&real);
    typename TypeParam::Device* device = nullptr;
    ASSERT_EQ(dinput->CreateDevice(GUID_Joystick, &device, nullptr), static_cast<HRESULT>(DI_OK));
    ASSERT_NE(device, nullptr);
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    ASSERT_EQ(device->GetCapabilities(&caps), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(GET_DIDEVICE_TYPE(caps.dwDevType), static_cast<DWORD>(DI8DEVTYPE_DRIVING));   // emulated
    EXPECT_EQ(device->Release(), 0U);

    real.device.references = 1;
    real.device.product = test::productGuid(0x046D, 0xC52B);   // not the wheel
    ASSERT_EQ(dinput->CreateDevice(GUID_SysMouse, &device, nullptr), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(device->GetCapabilities(&caps), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(GET_DIDEVICE_TYPE(caps.dwDevType), static_cast<DWORD>(DI8DEVTYPE_JOYSTICK));   // as it is
    EXPECT_EQ(device->Release(), 0U);

    real.device.references = 1;
    real.device.product = test::productGuid(0x346E, 0x0005);
    real.device.infoResult = DIERR_INPUTLOST;   // cannot say what it is: logged and passed through
    ASSERT_EQ(dinput->CreateDevice(GUID_Joystick, &device, nullptr), static_cast<HRESULT>(DI_OK));
    ASSERT_EQ(device->GetCapabilities(&caps), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(GET_DIDEVICE_TYPE(caps.dwDevType), static_cast<DWORD>(DI8DEVTYPE_JOYSTICK));
    EXPECT_EQ(device->Release(), 0U);
    real.device.infoResult = DI_OK;

    real.device.references = 1;
    ASSERT_EQ(dinput->CreateDevice(GUID_Joystick, &device, reinterpret_cast<LPUNKNOWN>(&real)), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(device, &real.device);   // aggregated: not wrapped
    EXPECT_EQ(dinput->CreateDevice(GUID_Joystick, nullptr, nullptr), static_cast<HRESULT>(DI_OK));
    real.result = DIERR_DEVICENOTREG;
    EXPECT_EQ(dinput->CreateDevice(GUID_Joystick, &device, nullptr), static_cast<HRESULT>(DIERR_DEVICENOTREG));
    real.result = DI_OK;

    // The rest goes to the real object, logged.
    void* object = nullptr;
    EXPECT_EQ(dinput->QueryInterface(IID_IUnknown, nullptr), static_cast<HRESULT>(E_POINTER));
    EXPECT_EQ(dinput->QueryInterface(TypeParam::directInputIid(), &object), static_cast<HRESULT>(S_OK));
    EXPECT_EQ(dinput->Release(), 1U);
    EXPECT_EQ(dinput->QueryInterface(IID_IDirectInputEffect, &object), static_cast<HRESULT>(E_NOINTERFACE));
    for (int i = 0; i < 3; ++i) EXPECT_EQ(dinput->GetDeviceStatus(GUID_Joystick), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(dinput->RunControlPanel(nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(dinput->Initialize(nullptr, 0x800), static_cast<HRESULT>(DI_OK));
    GUID found{};
    EXPECT_EQ(dinput->FindDevice(GUID_Joystick, S("path"), &found), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(dinput->FindDevice(GUID_Joystick, S("path"), nullptr), static_cast<HRESULT>(DI_OK));
    real.result = DIERR_DEVICENOTREG;
    EXPECT_EQ(dinput->FindDevice(GUID_Joystick, S("path"), &found), static_cast<HRESULT>(DIERR_DEVICENOTREG));
    real.result = DI_OK;
    EXPECT_EQ(dinput->EnumDevicesBySemantics(S("me"), nullptr, nullptr, nullptr, 0), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(dinput->ConfigureDevices(nullptr, nullptr, 0, nullptr), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(dinput->Release(), 0U);
}

// --- StateFeed -------------------------------------------------------------------------------------------------------

// The reads the game makes are counted and summed up at release.
TEST(ShimStateFeed, CountsReads) {
    StateFeed& feed = StateFeed::instance();
    const auto profile = makeProfile("g920");
    (void)feed.current(*profile);
    feed.noteRead(1'000, 2'000);
    feed.noteRead(70'000'000'000ULL, 70'000'001'000ULL);   // a minute later: the line is due
    feed.logReads();
    feed.logReads();   // nothing since: nothing to say
    SUCCEED();
}

}  // namespace
}  // namespace wheelbri::shim
