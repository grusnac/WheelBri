// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the shim's tests share (Windows only): the wheelbri-shim.ini they run with, a listener for the force feedback
// the shim sends, and fakes of DirectInput's objects (the "real" ones a proxy wraps), which record what they are asked
// and answer what a test sets.

#include "DiFormat.hpp"
#include "Traits.hpp"

#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wheelbri::shim::test {

// The ports in the wheelbri-shim.ini the tests write next to the test program (ShimDeviceTest.cpp).
inline constexpr std::uint16_t kStatePort = 47431;
inline constexpr std::uint16_t kFfbPort = 47432;

// The FfbPackets the shim sends to the service's port.
class FfbCatcher {
public:
    FfbCatcher() : listener_(UdpLoopbackListener::create(kFfbPort)) {}
    [[nodiscard]] bool ok() const { return listener_ != nullptr; }

    // The next packet of `kind` within `waitMs` (others before it are skipped), or nullopt.
    // Waits by the clock, not by counting sleeps: on Windows a 1 ms sleep takes up to 15.6 ms.
    std::optional<FfbPacket> next(const FfbPacket::Kind kind, const int waitMs = 500) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        while (true) {
            std::array<std::uint8_t, 128> buffer{};
            while (const auto size = listener_->receive(buffer)) {
                const auto packet = decodeFfbPacket(std::span<const std::uint8_t>(buffer.data(), *size));
                if (packet && packet->kind == kind) return packet;
            }
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void drain() {
        std::array<std::uint8_t, 128> buffer{};
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        while (listener_->receive(buffer)) {
        }
    }

private:
    std::unique_ptr<UdpLoopbackListener> listener_;
};

// A HID product GUID for vid:pid ("PIDVID").
inline GUID productGuid(const std::uint16_t vid, const std::uint16_t pid) {
    GUID guid{};
    guid.Data1 = static_cast<DWORD>(vid) | (static_cast<DWORD>(pid) << 16);
    const BYTE tail[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    std::memcpy(guid.Data4, tail, 8);
    return guid;
}

// A DirectInput effect that answers `result` to everything.
class FakeEffect final : public IDirectInputEffect {
public:
    HRESULT result = DI_OK;
    ULONG references = 1;
    int calls = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override {
        ++calls;
        if (object != nullptr) *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }   // owned by the test, never deleted
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return count(); }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID guid) override {
        if (guid != nullptr) *guid = GUID_Sine;
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE Start(DWORD, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE Stop() override { return count(); }
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD status) override {
        if (status != nullptr) *status = DIEGES_PLAYING;
        return count();
    }
    HRESULT STDMETHODCALLTYPE Download() override { return count(); }
    HRESULT STDMETHODCALLTYPE Unload() override { return count(); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return count(); }

private:
    HRESULT count() {
        ++calls;
        return result;
    }
};

// A name into a DirectInput string field, ANSI or wide.
template <std::size_t N>
void copyName(char (&out)[N], const char* name) {
    std::snprintf(out, N, "%s", name);
}
template <std::size_t N>
void copyName(wchar_t (&out)[N], const char* name) {
    std::size_t i = 0;
    for (; i + 1 < N && name[i] != '\0'; ++i) out[i] = static_cast<wchar_t>(name[i]);
    out[i] = L'\0';
}

// A DirectInput device, ANSI or wide (T: AnsiTraits / WideTraits): `result` from every call unless a test sets a
// call's own; GetDeviceInfo describes `product` (vid:pid) named "Moza R3"; enumerations offer one object / one effect.
template <typename T>
class FakeDeviceT final : public T::Device {
public:
    HRESULT result = DI_OK;
    HRESULT acquireResult = DI_OK;
    HRESULT dataFormatResult = DI_OK;
    HRESULT infoResult = DI_OK;
    DWORD infoSize = sizeof(typename T::DeviceInstance);
    GUID product = productGuid(0x346E, 0x0005);
    ULONG references = 1;
    int calls = 0;
    int dataFormats = 0;            // SetDataFormat calls
    DWORD cooperativeFlags = 0;     // the last SetCooperativeLevel's
    std::vector<std::string> properties;   // Get/SetProperty ids asked, "get N" / "set N"
    FakeEffect effect;              // what CreateEffect hands out
    bool effectOut = true;          // CreateEffect fills *out

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override {
        ++calls;
        if (object != nullptr) *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        if (caps != nullptr && result == DI_OK) {
            caps->dwFlags = DIDC_ATTACHED | DIDC_FORCEFEEDBACK;
            caps->dwDevType = DI8DEVTYPE_JOYSTICK | DIDEVTYPE_HID;
            caps->dwAxes = 8;
        }
        return count();
    }
    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjectsCallback callback, LPVOID ref, DWORD) override {
        typename T::DeviceObjectInstance object{};
        object.dwSize = sizeof(object);
        object.guidType = GUID_XAxis;
        copyName(object.tszName, "X");
        if (callback != nullptr) callback(&object, ref);
        object.dwSize = sizeof(typename T::DeviceObjectInstanceDx3);
        if (callback != nullptr) callback(&object, ref);
        if (callback != nullptr) callback(nullptr, ref);
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        properties.push_back("get " + std::to_string(reinterpret_cast<std::uintptr_t>(&property)));
        if (header != nullptr && header->dwSize == sizeof(DIPROPDWORD)) {
            reinterpret_cast<LPDIPROPDWORD>(header)->dwData = 1234;
        }
        return count();
    }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER) override {
        properties.push_back("set " + std::to_string(reinterpret_cast<std::uintptr_t>(&property)));
        return count();
    }
    HRESULT STDMETHODCALLTYPE Acquire() override {
        ++calls;
        return acquireResult;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return count(); }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        if (data != nullptr) std::memset(data, 0, size);
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD inOut, DWORD) override {
        if (inOut != nullptr) *inOut = 0;
        return count();
    }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT) override {
        ++calls;
        ++dataFormats;
        return dataFormatResult;
    }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return count(); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD flags) override {
        cooperativeFlags = flags;
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::DeviceObjectInstance* info, DWORD, DWORD) override {
        if (info != nullptr && result == DI_OK) {
            const DWORD size = info->dwSize;
            std::memset(info, 0, size);
            info->dwSize = size;
            copyName(info->tszName, "Y");
        }
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DeviceInstance* info) override {
        ++calls;
        if (info != nullptr && infoResult == DI_OK) {
            std::memset(info, 0, sizeof(*info));
            info->dwSize = infoSize;
            info->guidProduct = product;
            info->dwDevType = DI8DEVTYPE_JOYSTICK | DIDEVTYPE_HID;
            copyName(info->tszProductName, "Moza R3");
            copyName(info->tszInstanceName, "Moza R3");
        }
        return infoResult;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return count(); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        if (out != nullptr) *out = effectOut && result == DI_OK ? &effect : nullptr;
        return count();
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffectsCallback callback, LPVOID ref, DWORD) override {
        typename T::EffectInfo info{};
        info.dwSize = sizeof(info);
        info.guid = GUID_ConstantForce;
        copyName(info.tszName, "Constant");
        if (callback != nullptr) callback(&info, ref);
        if (callback != nullptr) callback(nullptr, ref);
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffectInfo*, REFGUID) override { return count(); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override {
        if (state != nullptr) *state = DIGFFS_STOPPED;
        return count();
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override {
        return count();
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return count(); }
    HRESULT STDMETHODCALLTYPE Poll() override { return count(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override {
        return count();
    }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::String, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
        return count();
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::String, DWORD, LPDIFILEEFFECT, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActionFormat, typename T::String, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActionFormat, typename T::String, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImageInfoHeader) override { return count(); }

private:
    HRESULT count() {
        ++calls;
        return result;
    }
};

// The IDirectInput8 a proxy wraps (ANSI or wide): CreateDevice hands out `device`; EnumDevices offers `instances`.
template <typename T>
class FakeDirectInputT final : public T::DirectInput {
public:
    HRESULT result = DI_OK;
    ULONG references = 1;
    int calls = 0;
    FakeDeviceT<T> device;
    std::vector<typename T::DeviceInstance> instances;
    DWORD lastFilter = 0;   // the device type EnumDevices was asked for

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override {
        ++calls;
        if (object != nullptr) *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID, typename T::Device** out, LPUNKNOWN) override {
        if (out != nullptr) *out = result == DI_OK ? &device : nullptr;
        return count();
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD filter, typename T::EnumDevicesCallback callback, LPVOID ref,
                                          DWORD) override {
        lastFilter = filter;
        for (const typename T::DeviceInstance& instance : instances) {
            if (callback(&instance, ref) == DIENUM_STOP) break;
        }
        return count();
    }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID) override { return count(); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) override { return count(); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID, typename T::String, LPGUID instance) override {
        if (instance != nullptr) *instance = GUID_Joystick;
        return count();
    }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::String, typename T::ActionFormat, typename T::EnumBySemanticsCallback,
                                                     LPVOID, DWORD) override {
        return count();
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, typename T::ConfigureParams, DWORD,
                                               LPVOID) override {
        return count();
    }

private:
    HRESULT count() {
        ++calls;
        return result;
    }
};

// A device instance as EnumDevices describes one (ANSI or wide).
template <typename T = WideTraits>
typename T::DeviceInstance instanceOf(const GUID& product, const char* name, const DWORD type,
                                      const DWORD size = sizeof(typename T::DeviceInstance)) {
    typename T::DeviceInstance instance{};
    instance.dwSize = size;
    instance.guidInstance = product;
    instance.guidInstance.Data1 ^= 0x1;
    instance.guidProduct = product;
    instance.dwDevType = type;
    copyName(instance.tszProductName, name);
    copyName(instance.tszInstanceName, name);
    return instance;
}

using FakeDevice = FakeDeviceT<WideTraits>;
using FakeDirectInput = FakeDirectInputT<WideTraits>;

}  // namespace wheelbri::shim::test
