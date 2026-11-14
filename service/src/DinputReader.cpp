// SPDX-License-Identifier: GPL-3.0-or-later
#include "DinputReader.hpp"

#include "wheelbri/Logging.hpp"
#include "wheelbri/WakePlanner.hpp"

#ifdef _WIN32

#include "DinputDevice.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

enum class Kind { Axis, Button, Hat };

struct Input {
    Kind kind;
    DWORD offset;    // field of DIJOYSTATE2, as SDL sorts them
};

struct EnumContext {
    IDirectInputDevice8W* device = nullptr;
    std::vector<Input> inputs;
    int buttons = 0;
    int hats = 0;
    int sliders = 0;
};

// Same rules as SDL3's EnumDevObjectsCallback (SDL_dinputjoystick.c), so the numbering stays what it was.
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
    HANDLE notification = nullptr;   // set by DirectInput when the state changes
    std::vector<Input> axes;       // SDL order
    std::vector<Input> buttons;
    std::vector<Input> hats;
    LossTimer loss;                  // 0.5 s of failed reads by the clock = gone (also when read a few times a second)
    ULONGLONG lastAcquireMs = 0;     // acquiring again at most every 50 ms

    ~Impl() {
        if (device != nullptr) {
            device->Unacquire();
            if (notification != nullptr) device->SetEventNotification(nullptr);
            device->Release();
        }
        if (notification != nullptr) CloseHandle(notification);
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
    impl->device = dinput::createByPath(impl->dinput, hidPath);
    if (impl->device == nullptr) {
        error = "no DirectInput game controller has the path " + hidPath;
        return nullptr;
    }

    // Shared, and also while another window has the focus. The force feedback holds the same wheel
    // exclusively (a second device object); exclusive access only excludes other exclusive users.
    impl->window = dinput::createHelperWindow(L"wheelbri-input");
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
    // A notification when the state changes, for the service loop; set before Acquire (DirectInput requires it).
    // Polled devices never set it: none then, and the loop polls.
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(impl->device->GetCapabilities(&caps)) && (caps.dwFlags & DIDC_POLLEDDEVICE) == 0) {
        impl->notification = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (impl->notification == nullptr) {
            WE_LOG_WARN(log(), "cannot create the event for DirectInput's notification (error {}): the loop polls",
                        GetLastError());
        } else if (const HRESULT set = impl->device->SetEventNotification(impl->notification); FAILED(set)) {
            WE_LOG_WARN(log(), "DirectInput refused the notification event (0x{:08x}): the loop polls",
                        static_cast<unsigned>(set));
            CloseHandle(impl->notification);
            impl->notification = nullptr;
        }
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
void* DinputReader::notification() const { return impl_->notification; }

bool DinputReader::read(RawInputState& state) {
    DIJOYSTATE2 s{};
    impl_->device->Poll();
    HRESULT result = impl_->device->GetDeviceState(sizeof(s), &s);
    // Acquire again at once, then every 50 ms at most: on an unplugged wheel it can take a while each time.
    const ULONGLONG nowMs = GetTickCount64();
    if ((result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED) &&
        (impl_->loss.failures() == 0 || nowMs - impl_->lastAcquireMs >= 50)) {
        impl_->lastAcquireMs = nowMs;
        const HRESULT acquired = impl_->device->Acquire();
        if (FAILED(acquired) && impl_->loss.failures() == 0) {
            WE_LOG_WARN(log(), "DirectInput: the wheel cannot be acquired again (0x{:08x}{})",
                        static_cast<unsigned>(acquired),
                        acquired == static_cast<HRESULT>(DIERR_UNPLUGGED) ? ", unplugged" : acquired == static_cast<HRESULT>(DIERR_OTHERAPPHASPRIO)
                                                                          ? ", another program has it exclusively"
                                                                          : "");
        }
        impl_->device->Poll();
        result = impl_->device->GetDeviceState(sizeof(s), &s);
    }
    if (FAILED(result)) {
        // Keep the last values; a wheel that stays unreadable for 0.5 s is gone.
        if (impl_->loss.failures() == 0) {
            WE_LOG_WARN(log(), "DirectInput read failed (0x{:08x}{}); keeping the last values",
                        static_cast<unsigned>(result), result == static_cast<HRESULT>(DIERR_UNPLUGGED) ? ", unplugged" : "");
        }
        return impl_->loss.failed(nowMs);
    }
    if (const std::uint64_t failed = impl_->loss.succeeded(); failed > 0) {
        WE_LOG_INFO(log(), "DirectInput reads again after {} failed", failed);
    }
    const std::size_t axisCount = std::min(impl_->axes.size(), RawInputState::kMaxAxes);
    for (std::size_t i = 0; i < axisCount; ++i) {
        const float value = static_cast<float>(axisValue(s, impl_->axes[i].offset)) / 32767.0F;
        state.axes[i] = std::clamp(value, -1.0F, 1.0F);
    }
    SourceButtons pressed;
    // All of DIJOYSTATE2's 128 (a Moza base reports its shifter's gears on 112..119).
    const std::size_t buttonCount = std::min<std::size_t>(impl_->buttons.size(), SourceButtons::kCount);
    for (std::size_t i = 0; i < buttonCount; ++i) {
        const std::size_t at = impl_->buttons[i].offset - DIJOFS_BUTTON0;
        if (at < std::size(s.rgbButtons) && s.rgbButtons[at] != 0) {
            pressed.set(static_cast<int>(i));
        }
    }
    state.buttons = pressed;
    state.hat = impl_->hats.empty() ? kHatCentered : hatIndex(s.rgdwPOV[(impl_->hats[0].offset - DIJOFS_POV(0)) / sizeof(DWORD)]);
    return true;
}

}  // namespace wheelbri::service

#endif  // _WIN32
