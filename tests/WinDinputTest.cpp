// The service's DirectInput helpers (service/src/DinputDevice.cpp), the reader of the wheel (DinputReader.cpp) and the
// wheel backend (WheelDinput.cpp), on the system's DirectInput. Nothing here opens a device that is there (the tests
// may run on a PC with a wheel plugged in): only listing, and opening what is not there. Windows only (MinGW: run under Wine, which lists no game controller).

#include "DinputDevice.hpp"
#include "DinputReader.hpp"
#include "Wheel.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wheelbri::service {
namespace {

// UTF-8 and UTF-16 both ways; empty stays empty.
TEST(WinDinput, TextBothWays) {
    EXPECT_EQ(dinput::toWide(""), L"");
    EXPECT_EQ(dinput::toWide("Moza R3 \xc3\xa9"), L"Moza R3 é");
    EXPECT_EQ(dinput::toUtf8(L""), "");
    EXPECT_EQ(dinput::toUtf8(L"G920 é"), "G920 \xc3\xa9");
}

// A device with no HID path (the system mouse) has none; a path no controller has finds nothing; the controllers
// listed each have a path.
TEST(WinDinput, PathsAndLookups) {
    IDirectInput8W* dinput = nullptr;
    ASSERT_TRUE(SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                             reinterpret_cast<void**>(&dinput), nullptr)));
    IDirectInputDevice8W* mouse = nullptr;
    if (SUCCEEDED(dinput->CreateDevice(GUID_SysMouse, &mouse, nullptr))) {
        (void)dinput::hidPath(mouse);   // a system mouse may or may not have one
        mouse->Release();
    }
    EXPECT_EQ(dinput::createByPath(dinput, "\\\\?\\hid#no-such-device"), nullptr);
    dinput->Release();
    for (const std::string& path : dinput::gameControllerPaths()) EXPECT_FALSE(path.empty());
    const HWND window = dinput::createHelperWindow(L"wheelbri-test");
    EXPECT_NE(window, nullptr);
    if (window != nullptr) DestroyWindow(window);
}

// Opening a wheel no DirectInput controller has fails and says which path.
TEST(WinDinput, ReaderForAMissingWheel) {
    std::string error;
    EXPECT_EQ(DinputReader::open("\\\\?\\hid#no-such-device", error), nullptr);
    EXPECT_NE(error.find("no DirectInput game controller has the path"), std::string::npos) << error;
}

// The DirectInput backend lists the game controllers (each with a name and a path; none under Wine) and refuses a
// wheel it has no path for, or whose path no controller has.
TEST(WinDinput, BackendListsAndRefusesMissingWheels) {
    const std::unique_ptr<WheelBackend> backend = startWheelBackend();
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->describe(), "DirectInput");
    for (const WheelInfo& info : backend->list()) {
        EXPECT_FALSE(info.name.empty());
        EXPECT_FALSE(info.path.empty());
    }
    WheelInfo missing;
    missing.name = "Moza R3";
    EXPECT_EQ(backend->open(missing), nullptr);   // no path
    missing.path = "\\\\?\\hid#no-such-device";
    EXPECT_EQ(backend->open(missing), nullptr);
}

}  // namespace
}  // namespace wheelbri::service
