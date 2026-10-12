#pragma once

// Windows only: finding a DirectInput game controller by its HID path (shared by DinputReader and the
// DirectInput force feedback in WheelDinput.cpp).

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dinput.h>

#include <string>
#include <vector>

namespace wheelbri::service::dinput {

[[nodiscard]] std::wstring toWide(const std::string& text);
[[nodiscard]] std::string toUtf8(const wchar_t* text);

// The device's HID path (DIPROP_GUIDANDPATH), empty if it has none.
[[nodiscard]] std::wstring hidPath(IDirectInputDevice8W* device);

// A new device object for the attached game controller with this HID path (case-insensitive), or nullptr.
[[nodiscard]] IDirectInputDevice8W* createByPath(IDirectInput8W* dinput, const std::string& path);

// The HID paths of every attached game controller DirectInput lists (what games see), through a DirectInput
// object of its own. Empty if DirectInput cannot be started.
[[nodiscard]] std::vector<std::string> gameControllerPaths();

// A hidden message-only window: DirectInput's SetCooperativeLevel needs one.
[[nodiscard]] HWND createHelperWindow(const wchar_t* title);

}  // namespace wheelbri::service::dinput
