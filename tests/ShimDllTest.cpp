// The built dinput8.dll shim as a game loads it (shim/src/Exports.cpp): a copy in a "game" folder of its own, loaded
// by its full path, its exports called through GetProcAddress in front of the system's real dinput8.dll. The
// shim pins itself on its first export call and its singletons live for the rest of the process, so one copy serves
// every test here. Its log goes to this suite's folder (%LOCALAPPDATA%, and XDG_STATE_HOME under Wine).
//
// Not called: DllRegisterServer / DllUnregisterServer, which would register the system's DirectInput again on the
// machine running the tests.
#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef WHEELBRI_SHIM_DLL
#error "WHEELBRI_SHIM_DLL: the built dinput8.dll shim"
#endif

namespace {

namespace fs = std::filesystem;

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using CanUnloadFn = HRESULT(WINAPI*)();
using ClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using FormatFn = LPCDIDATAFORMAT(WINAPI*)();

// The Unix form of a path under Wine, empty elsewhere (the shim reads XDG_STATE_HOME as a Unix path).
std::wstring unixPath(const fs::path& path) {
    using UnixNameFn = char*(__cdecl*)(const WCHAR*);
    const auto unixName = reinterpret_cast<UnixNameFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name")));
    if (unixName == nullptr) return {};
    char* const name = unixName(path.wstring().c_str());
    if (name == nullptr) return {};
    const std::string text(name);
    HeapFree(GetProcessHeap(), 0, name);
    return std::wstring(text.begin(), text.end());
}

class ShimDll : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = fs::temp_directory_path() / "wheelbri-shimdll";
        std::error_code ec;
        fs::remove_all(dir_, ec);   // a copy left loaded by an earlier run cannot be removed on Windows: fine
        fs::create_directories(dir_ / "game", ec);
        fs::create_directories(dir_ / "local", ec);
        SetEnvironmentVariableW(L"LOCALAPPDATA", (dir_ / "local").c_str());
        const std::wstring state = unixPath(dir_ / "local");
        SetEnvironmentVariableW(L"XDG_STATE_HOME", state.empty() ? nullptr : state.c_str());
        SetEnvironmentVariableW(L"XDG_CONFIG_HOME", state.empty() ? nullptr : state.c_str());
        // The target the G920, the wheel nowhere (no device has this vendor id); logging at trace (the app's level).
        std::ofstream(dir_ / "game" / "wheelbri-shim.ini") << "[shim]\ntarget_profile = g920\nsource_vid = 0001\n";
        fs::create_directories(dir_ / "local" / "wheelbri", ec);
        std::ofstream(dir_ / "local" / "wheelbri" / "app.ini") << "[app]\nlog_level = trace\n";
        // Under another name: the test program itself uses the system's dinput8.dll, and Wine hands back a DLL already
        // loaded under the same name whatever the path asked for.
        fs::copy_file(WHEELBRI_SHIM_DLL, dir_ / "game" / "wheelbri-dinput8.dll", fs::copy_options::overwrite_existing,
                      ec);
        module_ = LoadLibraryW((dir_ / "game" / "wheelbri-dinput8.dll").c_str());
    }

    void SetUp() override { ASSERT_NE(module_, nullptr) << "the shim did not load: " << GetLastError(); }

    template <typename Fn>
    [[nodiscard]] static Fn fn(const char* name) {
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module_, name)));
    }

    [[nodiscard]] static std::string logText() {
        std::string text;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(dir_ / "local", ec)) {
            if (entry.path().filename().string().rfind("wheelbri-shim", 0) == 0) {
                std::ifstream in(entry.path(), std::ios::binary);
                text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
        }
        return text;
    }

    static inline fs::path dir_;
    static inline HMODULE module_ = nullptr;
};

// DirectInput8Create hands out the shim's wrapper for both interfaces (a module of its own, not the system's), passes
// any other interface and an aggregated object through unwrapped (with a warning), and returns the real DLL's error
// as it is.
TEST_F(ShimDll, DirectInput8CreateWrapsWhatGamesUse) {
    const auto create = fn<CreateFn>("DirectInput8Create");
    ASSERT_NE(create, nullptr);
    const HINSTANCE self = GetModuleHandleW(nullptr);
    const auto ownerOf = [](void* object) {
        HMODULE owner = nullptr;
        const void* const method = (*reinterpret_cast<void***>(object))[0];   // QueryInterface
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(method), &owner);
        return owner;
    };

    void* wide = nullptr;
    ASSERT_EQ(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W, &wide, nullptr), DI_OK);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(ownerOf(wide), module_);
    void* ansi = nullptr;
    ASSERT_EQ(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8A, &ansi, nullptr), DI_OK);
    ASSERT_NE(ansi, nullptr);
    EXPECT_EQ(ownerOf(ansi), module_);
    static_cast<IDirectInput8W*>(wide)->Release();
    static_cast<IDirectInput8A*>(ansi)->Release();

    void* other = nullptr;
    const HRESULT unknown = create(self, DIRECTINPUT_VERSION, IID_IUnknown, &other, nullptr);
    if (SUCCEEDED(unknown) && other != nullptr) {
        EXPECT_NE(ownerOf(other), module_);   // not wrapped
        static_cast<IUnknown*>(other)->Release();
    }

    EXPECT_TRUE(FAILED(create(self, DIRECTINPUT_VERSION, IID_IDirectInput8W, nullptr, nullptr)));
    EXPECT_TRUE(FAILED(create(self, 0x0300, IID_IDirectInput8W, &other, nullptr)));   // a version it refuses

    const std::string log = logText();
    EXPECT_NE(log.find("DirectInput8Create(version 0x800"), std::string::npos) << log;
    EXPECT_NE(log.find("real DirectInput8Create ->"), std::string::npos);
    if (SUCCEEDED(unknown)) {
        EXPECT_NE(log.find("passing it through unwrapped"), std::string::npos);
    }
}

// The rest of what dinput8.dll exports goes to the real DLL: its class factory (logged: objects made that way are not
// the shim's), whether it may be unloaded, and the joystick data format.
TEST_F(ShimDll, TheOtherExportsGoToTheRealDll) {
    const auto classObject = fn<ClassObjectFn>("DllGetClassObject");
    ASSERT_NE(classObject, nullptr);
    void* factory = nullptr;
    const HRESULT made = classObject(CLSID_DirectInput8, IID_IClassFactory, &factory);
    if (SUCCEEDED(made) && factory != nullptr) static_cast<IUnknown*>(factory)->Release();
    const auto canUnload = fn<CanUnloadFn>("DllCanUnloadNow");
    ASSERT_NE(canUnload, nullptr);
    const HRESULT unload = canUnload();
    EXPECT_TRUE(unload == S_OK || unload == S_FALSE);
    const auto format = fn<FormatFn>("GetdfDIJoystick");
    ASSERT_NE(format, nullptr);
    const LPCDIDATAFORMAT joystick = format();
    if (joystick != nullptr) {
        EXPECT_EQ(joystick->dwSize, sizeof(DIDATAFORMAT));
    }
    EXPECT_NE(logText().find("objects created this way are NOT proxied"), std::string::npos);
}

// A game's look at its devices through the shim: every device the system has is listed (and logged), none is the
// wheel (no device has the configured vendor id), and a device the game opens is the shim's pass-through wrapper.
TEST_F(ShimDll, ListsAndOpensTheSystemsDevices) {
    const auto create = fn<CreateFn>("DirectInput8Create");
    ASSERT_NE(create, nullptr);
    IDirectInput8W* input = nullptr;
    ASSERT_EQ(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                     reinterpret_cast<void**>(&input), nullptr),
              DI_OK);
    int listed = 0;
    const auto count = [](LPCDIDEVICEINSTANCEW, LPVOID context) -> BOOL {
        ++*static_cast<int*>(context);
        return DIENUM_CONTINUE;
    };
    EXPECT_EQ(input->EnumDevices(DI8DEVCLASS_ALL, count, &listed, DIEDFL_ALLDEVICES), DI_OK);
    EXPECT_EQ(input->EnumDevices(DI8DEVCLASS_GAMECTRL, count, &listed, DIEDFL_ATTACHEDONLY), DI_OK);
    int stopped = 0;
    const auto stopAtFirst = [](LPCDIDEVICEINSTANCEW, LPVOID context) -> BOOL {
        ++*static_cast<int*>(context);
        return DIENUM_STOP;
    };
    EXPECT_EQ(input->EnumDevices(DI8DEVCLASS_ALL, stopAtFirst, &stopped, DIEDFL_ALLDEVICES), DI_OK);
    EXPECT_LE(stopped, 1);

    IDirectInputDevice8W* keyboard = nullptr;
    if (SUCCEEDED(input->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)) && keyboard != nullptr) {
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        EXPECT_EQ(keyboard->GetCapabilities(&caps), DI_OK);
        EXPECT_EQ(keyboard->SetDataFormat(&c_dfDIKeyboard), DI_OK);
        keyboard->Release();
    }
    input->Release();
    const std::string log = logText();
    EXPECT_NE(log.find("EnumDevices"), std::string::npos) << log;
}

}  // namespace
