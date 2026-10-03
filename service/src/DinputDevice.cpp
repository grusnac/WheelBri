#include "DinputDevice.hpp"

#include <algorithm>
#include <cwctype>

namespace wheelbri::service::dinput {

namespace {

bool samePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

struct FindContext {
    IDirectInput8W* dinput;
    std::wstring path;
    IDirectInputDevice8W* found = nullptr;
};

BOOL CALLBACK findDevice(LPCDIDEVICEINSTANCEW instance, LPVOID context) {
    auto* find = static_cast<FindContext*>(context);
    IDirectInputDevice8W* device = nullptr;
    if (FAILED(find->dinput->CreateDevice(instance->guidInstance, &device, nullptr))) {
        return DIENUM_CONTINUE;
    }
    if (samePath(hidPath(device), find->path)) {
        find->found = device;
        return DIENUM_STOP;
    }
    device->Release();
    return DIENUM_CONTINUE;
}

}  // namespace

std::wstring toWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string toUtf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(length - 1));
    return out;
}

std::wstring hidPath(IDirectInputDevice8W* device) {
    DIPROPGUIDANDPATH property{};
    property.diph.dwSize = sizeof(property);
    property.diph.dwHeaderSize = sizeof(property.diph);
    property.diph.dwHow = DIPH_DEVICE;
    if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &property.diph))) {
        return {};
    }
    return property.wszPath;
}

IDirectInputDevice8W* createByPath(IDirectInput8W* dinput, const std::string& path) {
    FindContext find{dinput, toWide(path)};
    dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, findDevice, &find, DIEDFL_ATTACHEDONLY);
    return find.found;
}

HWND createHelperWindow(const wchar_t* title) {
    return CreateWindowExW(0, L"STATIC", title, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                           nullptr);
}

}  // namespace wheelbri::service::dinput
