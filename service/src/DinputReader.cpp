#include "DinputReader.hpp"

#include "wheelemu/Logging.hpp"

#ifdef _WIN32

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

std::wstring toWide(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(wcslen(wide.c_str()));
    return wide;
}

bool samePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

enum class Kind { Axis, Button, Hat };

struct Input {
    Kind kind;
    DWORD offset;    // field of DIJOYSTATE2, as SDL sorts them
};

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
    DIPROPGUIDANDPATH property{};
    property.diph.dwSize = sizeof(property);
    property.diph.dwHeaderSize = sizeof(property.diph);
    property.diph.dwHow = DIPH_DEVICE;
    if (SUCCEEDED(device->GetProperty(DIPROP_GUIDANDPATH, &property.diph)) && samePath(property.wszPath, find->path)) {
        find->found = device;
        return DIENUM_STOP;
    }
    device->Release();
    return DIENUM_CONTINUE;
}

struct EnumContext {
    IDirectInputDevice8W* device = nullptr;
    std::vector<Input> inputs;
    int buttons = 0;
    int hats = 0;
    int sliders = 0;
};

// Same rules as SDL's EnumDevObjectsCallback (SDL_dinputjoystick.c).
BOOL CALLBACK collectObject(LPCDIDEVICEOBJECTINSTANCEW object, LPVOID context) {
    auto* e = static_cast<EnumContext*>(context);
    if ((object->dwType & DIDFT_BUTTON) != 0) {
        e->inputs.push_back({Kind::Button, static_cast<DWORD>(DIJOFS_BUTTON(e->buttons++))});
    } else if ((object->dwType & DIDFT_POV) != 0) {
        e->inputs.push_back({Kind::Hat, static_cast<DWORD>(DIJOFS_POV(e->hats++))});
    } else if ((object->dwType & DIDFT_AXIS) != 0) {
        DWORD offset = 0;
        const GUID& type = object->guidType;
        if (type == GUID_XAxis) {
            offset = DIJOFS_X;
        } else if (type == GUID_YAxis) {
            offset = DIJOFS_Y;
        } else if (type == GUID_ZAxis) {
            offset = DIJOFS_Z;
        } else if (type == GUID_RxAxis) {
            offset = DIJOFS_RX;
        } else if (type == GUID_RyAxis) {
            offset = DIJOFS_RY;
        } else if (type == GUID_RzAxis) {
            offset = DIJOFS_RZ;
        } else if (type == GUID_Slider) {
            offset = static_cast<DWORD>(DIJOFS_SLIDER(e->sliders++));
        } else {
            return DIENUM_CONTINUE;
        }
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(range.diph);
        range.diph.dwObj = object->dwType;
        range.diph.dwHow = DIPH_BYID;
        range.lMin = -32768;
        range.lMax = 32767;
        if (FAILED(e->device->SetProperty(DIPROP_RANGE, &range.diph))) {
            return DIENUM_CONTINUE;
        }
        DIPROPDWORD deadzone{};
        deadzone.diph.dwSize = sizeof(deadzone);
        deadzone.diph.dwHeaderSize = sizeof(deadzone.diph);
        deadzone.diph.dwObj = object->dwType;
        deadzone.diph.dwHow = DIPH_BYID;
        deadzone.dwData = 0;
        if (FAILED(e->device->SetProperty(DIPROP_DEADZONE, &deadzone.diph))) {
            return DIENUM_CONTINUE;
        }
        e->inputs.push_back({Kind::Axis, offset});
    }
    return DIENUM_CONTINUE;
}

LONG axisValue(const DIJOYSTATE2& s, DWORD offset) {
    switch (offset) {
        case DIJOFS_X: return s.lX;
        case DIJOFS_Y: return s.lY;
        case DIJOFS_Z: return s.lZ;
        case DIJOFS_RX: return s.lRx;
        case DIJOFS_RY: return s.lRy;
        case DIJOFS_RZ: return s.lRz;
        case DIJOFS_SLIDER(0): return s.rglSlider[0];
        case DIJOFS_SLIDER(1): return s.rglSlider[1];
        default: return 0;
    }
}

// POV in hundredths of a degree (0xFFFF.. = centred) -> 0..7 clockwise from up, like SDL's hat.
std::int8_t hatIndex(DWORD pov) {
    if (LOWORD(pov) == 0xFFFF) {
        return kHatCentered;
    }
    return static_cast<std::int8_t>(((pov + 2250) / 4500) % 8);
}

}  // namespace

struct DinputReader::Impl {
    IDirectInput8W* dinput = nullptr;
    IDirectInputDevice8W* device = nullptr;
    HWND window = nullptr;
    std::vector<Input> axes;       // SDL order
    std::vector<Input> buttons;
    std::vector<Input> hats;
    int failures = 0;

    ~Impl() {
        if (device != nullptr) {
            device->Unacquire();
            device->Release();
        }
        if (dinput != nullptr) {
            dinput->Release();
        }
        if (window != nullptr) {
            DestroyWindow(window);
        }
    }
};

std::unique_ptr<DinputReader> DinputReader::open(const std::string& hidPath, std::string& error) {
    auto impl = std::make_unique<Impl>();
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                  reinterpret_cast<void**>(&impl->dinput), nullptr))) {
        error = "DirectInput8Create failed";
        return nullptr;
    }
    FindContext find{impl->dinput, toWide(hidPath)};
    impl->dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, findDevice, &find, DIEDFL_ATTACHEDONLY);
    if (find.found == nullptr) {
        error = "no DirectInput game controller has the path " + hidPath;
        return nullptr;
    }
    impl->device = find.found;

    // Shared, and also while another window has the focus. SDL holds the same wheel exclusively for force
    // feedback; exclusive access only excludes other exclusive users.
    impl->window = CreateWindowExW(0, L"STATIC", L"wheelemu-input", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    if (impl->window == nullptr ||
        FAILED(impl->device->SetCooperativeLevel(impl->window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
        error = "SetCooperativeLevel(non-exclusive, background) failed";
        return nullptr;
    }
    if (FAILED(impl->device->SetDataFormat(&c_dfDIJoystick2))) {
        error = "SetDataFormat(DIJOYSTATE2) failed";
        return nullptr;
    }
    EnumContext e;
    e.device = impl->device;
    impl->device->EnumObjects(collectObject, &e, DIDFT_BUTTON | DIDFT_AXIS | DIDFT_POV);
    std::stable_sort(e.inputs.begin(), e.inputs.end(), [](const Input& a, const Input& b) { return a.offset < b.offset; });
    for (const Input& input : e.inputs) {
        (input.kind == Kind::Axis ? impl->axes : input.kind == Kind::Button ? impl->buttons : impl->hats).push_back(input);
    }
    if (FAILED(impl->device->Acquire())) {
        error = "Acquire failed";
        return nullptr;
    }
    return std::unique_ptr<DinputReader>(new DinputReader(std::move(impl)));
}

DinputReader::DinputReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DinputReader::~DinputReader() = default;

int DinputReader::axes() const { return static_cast<int>(impl_->axes.size()); }
int DinputReader::buttons() const { return static_cast<int>(impl_->buttons.size()); }
int DinputReader::hats() const { return static_cast<int>(impl_->hats.size()); }

bool DinputReader::read(RawInputState& state) {
    DIJOYSTATE2 s{};
    impl_->device->Poll();
    HRESULT result = impl_->device->GetDeviceState(sizeof(s), &s);
    if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) {
        impl_->device->Acquire();
        impl_->device->Poll();
        result = impl_->device->GetDeviceState(sizeof(s), &s);
    }
    if (FAILED(result)) {
        // Keep the last values; a wheel that stays unreadable for ~0.5 s (at 1 kHz) is gone.
        if (++impl_->failures == 1) {
            WE_LOG_WARN(log(), "DirectInput read failed (0x{:08x}); keeping the last values", static_cast<unsigned>(result));
        }
        return impl_->failures < 500;
    }
    if (impl_->failures > 0) {
        WE_LOG_INFO(log(), "DirectInput reads again after {} failed", impl_->failures);
        impl_->failures = 0;
    }
    const std::size_t axisCount = std::min(impl_->axes.size(), RawInputState::kMaxAxes);
    for (std::size_t i = 0; i < axisCount; ++i) {
        const float value = static_cast<float>(axisValue(s, impl_->axes[i].offset)) / 32767.0F;
        state.axes[i] = std::clamp(value, -1.0F, 1.0F);
    }
    std::uint64_t pressed = 0;
    const std::size_t buttonCount = std::min<std::size_t>(impl_->buttons.size(), 64);
    for (std::size_t i = 0; i < buttonCount; ++i) {
        if (s.rgbButtons[impl_->buttons[i].offset - DIJOFS_BUTTON0] != 0) {
            pressed |= 1ULL << i;
        }
    }
    state.buttons = pressed;
    state.hat = impl_->hats.empty() ? kHatCentered : hatIndex(s.rgdwPOV[(impl_->hats[0].offset - DIJOFS_POV(0)) / sizeof(DWORD)]);
    return true;
}

}  // namespace wheelemu::service

#else  // not Windows: SDL reads the wheel.

namespace wheelemu::service {

struct DinputReader::Impl {};

std::unique_ptr<DinputReader> DinputReader::open(const std::string&, std::string& error) {
    error = "DirectInput exists on Windows only";
    return nullptr;
}

DinputReader::DinputReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DinputReader::~DinputReader() = default;
int DinputReader::axes() const { return 0; }
int DinputReader::buttons() const { return 0; }
int DinputReader::hats() const { return 0; }
bool DinputReader::read(RawInputState&) { return false; }

}  // namespace wheelemu::service

#endif
