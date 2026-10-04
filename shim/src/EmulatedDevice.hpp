#pragma once

#include "DiConvert.hpp"
#include "EmulatedEffect.hpp"
#include "FfbForwarder.hpp"
#include "Guard.hpp"
#include "ProxyEffect.hpp"
#include "RealDInput.hpp"
#include "ShimConfig.hpp"
#include "ShimLogging.hpp"
#include "StateFeed.hpp"
#include "Traits.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/EmulatedJoystick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri::shim {

namespace detail {

inline const logging::Logger& emulationLog() {
    static const logging::Logger instance = logger("shim.emu");
    return instance;
}

}  // namespace detail

// HID devices get a product GUID of the form {PIDVID-0000-0000-0000-504944564944} ("PIDVID").
[[nodiscard]] inline bool readVidPid(const GUID& product, std::uint16_t& vendorId, std::uint16_t& productId) {
    static constexpr BYTE kPidVid[8] = {0x00, 0x00, 'P', 'I', 'D', 'V', 'I', 'D'};
    if (product.Data2 != 0 || product.Data3 != 0 || std::memcmp(product.Data4, kPidVid, sizeof(kPidVid)) != 0) {
        return false;
    }
    vendorId = LOWORD(product.Data1);
    productId = HIWORD(product.Data1);
    return true;
}

// True if DirectInput describes the physical wheel the config says to replace.
template <typename Instance>
[[nodiscard]] bool isSourceDevice(const Instance& instance) {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    return readVidPid(instance.guidProduct, vendorId, productId) && shimConfig().matchesSource(vendorId, productId);
}

// Rewrites what DirectInput says about the physical wheel so it reads as the emulated one. The instance
// GUID stays the real one, so CreateDevice still opens the physical device (which the shim then wraps).
// Only fields that exist in the DirectX 3 struct layout are touched, so short structs are safe.
template <typename Instance>
void presentAs(Instance& instance, const DeviceIdentity& identity) {
    instance.guidProduct = toGuid(identity.directInputProductGuid());
    instance.dwDevType =
        DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_THREEPEDALS << 8) | (instance.dwDevType & DIDEVTYPE_HID);
    copyText(instance.tszProductName, identity.productName);
    copyText(instance.tszInstanceName, identity.productName);
}

// Phase 2: the physical wheel presented as the target wheel (G920). Input comes from wheelbri-service
// through StateFeed, in the target's layout and the ranges the game asked for. Force feedback and device
// management (acquire, cooperative level, events) are forwarded to the physical wheel.
template <typename T>
class EmulatedDevice final : public T::Device {
public:
    EmulatedDevice(typename T::Device* real, std::unique_ptr<IDeviceProfile> profile, std::string realName)
        : real_(real),
          profile_(std::move(profile)),
          joystick_(shimConfig().layout(countButtons(*profile_))),
          label_("[" + profile_->identity().shortName + " emulated over '" + realName + "']") {
        lastReport_ = profile_->encode(WheelState{});
        WE_LOG_INFO(detail::emulationLog(), "{}: device created ({}): {} axes, {} buttons, {} POV", label_,
                    T::kSuffix, joystick_.axisCount(), joystick_.buttonCount(), joystick_.povCount());
    }

    EmulatedDevice(const EmulatedDevice&) = delete;
    EmulatedDevice& operator=(const EmulatedDevice&) = delete;

    // --- IUnknown ---------------------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == T::deviceIid()) {
            *object = this;
            AddRef();
            return S_OK;
        }
        const HRESULT result = real_->QueryInterface(riid, object);
        guardedVoid("QueryInterface", [&] {
            WE_LOG_WARN(detail::emulationLog(),
                        "{}: QueryInterface({}) handed out the PHYSICAL device -> {}; the game may see the real wheel",
                        label_, describeGuid(riid), describeHresult(result));
        });
        return result;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            // Released without Unacquire (or with effects still started): its forces must not play on, nor come
            // back when the game opens the wheel again. Only this device's: the game may have the wheel open twice.
            if (serviceFfb()) {
                try {
                    FfbForwarder::instance().stopAll(this);
                } catch (...) {   // never let anything escape into the game
                }
            }
            real_->Release();
            guardedVoid("Release", [&] {
                WE_LOG_INFO(detail::emulationLog(), "{}: released after {} GetDeviceState / {} GetDeviceData calls",
                            label_, stateCalls_.load(), dataCalls_.load());
            });
            delete this;
        }
        return remaining;
    }

    // --- Description ------------------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        return guarded("GetCapabilities", [&]() -> HRESULT {
            if (caps == nullptr || caps->dwSize < sizeof(DIDEVCAPS_DX3)) {
                return DIERR_INVALIDPARAM;
            }
            const HRESULT result = real_->GetCapabilities(caps);   // keeps the real force feedback capabilities
            if (SUCCEEDED(result)) {
                caps->dwDevType =
                    DI8DEVTYPE_DRIVING | (DI8DEVTYPEDRIVING_THREEPEDALS << 8) | (caps->dwDevType & DIDEVTYPE_HID);
                caps->dwAxes = static_cast<DWORD>(joystick_.axisCount());
                caps->dwButtons = static_cast<DWORD>(joystick_.buttonCount());
                caps->dwPOVs = static_cast<DWORD>(joystick_.povCount());
                if (shimConfig().forceFeedback == FfbMode::Off) {
                    caps->dwFlags &= ~static_cast<DWORD>(DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE |
                                                         DIDC_SATURATION | DIDC_POSNEGCOEFFICIENTS |
                                                         DIDC_POSNEGSATURATION | DIDC_DEADBAND | DIDC_STARTDELAY);
                }
            }
            WE_LOG_INFO(detail::emulationLog(), "{}: GetCapabilities -> {} {}", label_, describeHresult(result),
                        SUCCEEDED(result) ? describeCaps(*caps) : std::string());
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DeviceInstance* info) override {
        return guarded("GetDeviceInfo", [&]() -> HRESULT {
            const HRESULT result = real_->GetDeviceInfo(info);
            if (SUCCEEDED(result) && info != nullptr) {
                presentAs(*info, profile_->identity());
            }
            WE_LOG_INFO(detail::emulationLog(), "{}: GetDeviceInfo -> {}", label_, describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjectsCallback callback, LPVOID ref, DWORD flags) override {
        return guarded("EnumObjects", [&]() -> HRESULT {
            if (callback == nullptr) {
                return DIERR_INVALIDPARAM;
            }
            std::vector<typename T::DeviceObjectInstance> list;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                const DWORD kinds = flags & (DIDFT_AXIS | DIDFT_BUTTON | DIDFT_POV);
                for (std::size_t i = 0; i < joystick_.objects().size(); ++i) {
                    const EmulatedObject& object = joystick_.objects()[i];
                    if (kinds != 0 && (kinds & object.typeId) == 0) {
                        continue;
                    }
                    if ((flags & DIDFT_FFACTUATOR) != 0 && (object.typeId & DIDFT_FFACTUATOR) == 0) {
                        continue;
                    }
                    if ((flags & DIDFT_FFEFFECTTRIGGER) != 0) {
                        continue;   // no trigger buttons
                    }
                    list.push_back(describe(i));
                }
            }
            std::size_t delivered = 0;
            for (const auto& object : list) {
                ++delivered;
                if (callback(&object, ref) == DIENUM_STOP) {
                    break;
                }
            }
            WE_LOG_INFO(detail::emulationLog(), "{}: EnumObjects(flags=0x{:x}) -> {} of {} objects", label_, flags,
                        delivered, list.size());
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::DeviceObjectInstance* info, DWORD object, DWORD how) override {
        return guarded("GetObjectInfo", [&]() -> HRESULT {
            // Only the two sizes DirectInput knows (DirectX 3 and current), as the real DirectInput checks.
            if (info == nullptr || (info->dwSize != sizeof(typename T::DeviceObjectInstance) &&
                                    info->dwSize != sizeof(typename T::DeviceObjectInstanceDx3))) {
                return DIERR_INVALIDPARAM;
            }
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto index = lookup(object, how);
            if (!index) {
                WE_LOG_DEBUG(detail::emulationLog(), "{}: GetObjectInfo(obj=0x{:x}, how={}) -> not found", label_,
                             object, how);
                return DIERR_OBJECTNOTFOUND;
            }
            const auto full = describe(*index);
            const DWORD size = info->dwSize;
            std::memcpy(info, &full, size);
            info->dwSize = size;
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        return guarded("GetProperty", [&]() -> HRESULT {
            if (header == nullptr || header->dwHeaderSize != sizeof(DIPROPHEADER)) {
                return DIERR_INVALIDPARAM;
            }
            const HRESULT result = getProperty(property, header);
            // Some games ask every frame, and every shim line is flushed to disk: sampled (failures always).
            if (const auto call = propertySampler_.next(); call != 0 || FAILED(result)) {
                WE_LOG_DEBUG(detail::emulationLog(), "{}: GetProperty({}, how={}, obj=0x{:x}) -> {} {} (#{})", label_,
                             describeProperty(property), header->dwHow, header->dwObj, describeHresult(result),
                             SUCCEEDED(result) ? describePropertyValue(property, header) : std::string(), call);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override {
        return guarded("SetProperty", [&]() -> HRESULT {
            if (header == nullptr || header->dwHeaderSize != sizeof(DIPROPHEADER)) {
                return DIERR_INVALIDPARAM;
            }
            const HRESULT result = setProperty(property, header);
            if (const auto call = setPropertySampler_.next(); call != 0 || FAILED(result)) {   // e.g. the gain every frame
                WE_LOG_INFO(detail::emulationLog(), "{}: SetProperty({}, how={}, obj=0x{:x}, {}) -> {} (#{})", label_,
                            describeProperty(property), header->dwHow, header->dwObj,
                            describePropertyValue(property, header), describeHresult(result), call);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImageInfoHeader) override {
        return guarded("GetImageInfo", [&]() -> HRESULT {
            WE_LOG_INFO(detail::emulationLog(), "{}: GetImageInfo -> unsupported", label_);
            return DIERR_UNSUPPORTED;
        });
    }

    // --- Setup ------------------------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override {
        return guarded("SetDataFormat", [&]() -> HRESULT {
            if (format == nullptr || format->dwSize != sizeof(DIDATAFORMAT) ||
                format->dwObjSize != sizeof(DIOBJECTDATAFORMAT) ||
                (format->dwNumObjs != 0 && format->rgodf == nullptr)) {
                return DIERR_INVALIDPARAM;
            }
            HRESULT result = DI_OK;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (acquired_) {
                    result = DIERR_ACQUIRED;
                } else if (joystick_.setDataFormat(toEntries(*format), format->dwDataSize) !=
                           EmulatedJoystick::FormatResult::Ok) {
                    result = DIERR_INVALIDPARAM;
                }
            }
            WE_LOG_INFO(detail::emulationLog(), "{}: SetDataFormat({}) -> {}", label_, describeDataFormat(format),
                        describeHresult(result));
            if (SUCCEEDED(result)) {
                logAssignment();
                // The physical device needs a format too, so force feedback axes given as offsets resolve.
                // (c_dfDIJoystick2 lives in dinput8.lib, which this DLL must not link; the real DLL exports
                // c_dfDIJoystick through GetdfDIJoystick.)
                const HRESULT realResult = real_->SetDataFormat(format);
                if (FAILED(realResult)) {
                    const RealDInput* const realDll = realDInput();
                    const LPCDIDATAFORMAT standard = realDll != nullptr && realDll->getdfDIJoystick != nullptr
                                                         ? realDll->getdfDIJoystick()
                                                         : nullptr;
                    const HRESULT fallback = standard != nullptr ? real_->SetDataFormat(standard) : DIERR_UNSUPPORTED;
                    WE_LOG_WARN(detail::emulationLog(),
                                "{}: physical wheel rejected the game's data format ({}), using c_dfDIJoystick -> {}",
                                label_, describeHresult(realResult), describeHresult(fallback));
                }
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override {
        return guarded("SetCooperativeLevel", [&]() -> HRESULT {
            if (serviceFfb()) {
                // wheelbri-service owns the physical wheel's force feedback (exclusive access), so the game's
                // handle on it must stay non-exclusive. Input comes from the service anyway.
                const HRESULT result = real_->SetCooperativeLevel(window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
                WE_LOG_INFO(detail::emulationLog(), "{}: SetCooperativeLevel(hwnd={}, {}) -> physical wheel opened "
                                                    "NONEXCLUSIVE|BACKGROUND -> {}",
                            label_, static_cast<void*>(window), describeCooperativeLevel(flags),
                            describeHresult(result));
                return DI_OK;
            }
            const HRESULT result = real_->SetCooperativeLevel(window, flags);
            WE_LOG_INFO(detail::emulationLog(), "{}: SetCooperativeLevel(hwnd={}, {}) -> {}", label_,
                        static_cast<void*>(window), describeCooperativeLevel(flags), describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override {
        return guarded("SetEventNotification", [&]() -> HRESULT {
            // The physical wheel signals whenever it changes, which is when the service's state changes too.
            return real_->SetEventNotification(event);
        });
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override {
        return guarded("Initialize", [&]() -> HRESULT {
            return real_->Initialize(instance, version, guid);
        });
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND owner, DWORD flags) override {
        return guarded("RunControlPanel", [&]() -> HRESULT {
            return real_->RunControlPanel(owner, flags);
        });
    }

    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActionFormat, typename T::String, DWORD) override {
        return guarded("BuildActionMap", [&]() -> HRESULT {
            WE_LOG_WARN(detail::emulationLog(), "{}: BuildActionMap (action mapping) is not emulated", label_);
            return DIERR_UNSUPPORTED;
        });
    }

    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActionFormat, typename T::String, DWORD) override {
        return guarded("SetActionMap", [&]() -> HRESULT {
            WE_LOG_WARN(detail::emulationLog(), "{}: SetActionMap (action mapping) is not emulated", label_);
            return DIERR_UNSUPPORTED;
        });
    }

    // --- Acquisition and reading --------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE Acquire() override {
        return guarded("Acquire", [&]() -> HRESULT {
            HRESULT result = real_->Acquire();
            if (serviceFfb() && FAILED(result)) {
                // Not needed for input or force feedback in service mode; never let it block the game. Games
                // acquire every frame, so this is rate limited.
                std::uint64_t suppressed = 0;
                if (physicalAcquireLimiter_.allow(suppressed)) {
                    WE_LOG_DEBUG(detail::emulationLog(), "{}: physical wheel Acquire -> {} (ignored) ({} similar "
                                                         "suppressed)",
                                 label_, describeHresult(result), suppressed);
                }
                result = DI_OK;
            }
            if (SUCCEEDED(result)) {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (!acquired_) {
                    acquired_ = true;
                    buffer_.clear();
                    lastReport_ = StateFeed::instance().current(*profile_);   // buffered data starts from now
                    WE_LOG_INFO(detail::emulationLog(), "{}: acquired", label_);
                }
            } else {
                WE_LOG_WARN_LIMITED(detail::emulationLog(), acquireLimiter_, "{}: Acquire -> {}", label_,
                                    describeHresult(result));
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Unacquire() override {
        return guarded("Unacquire", [&]() -> HRESULT {
            HRESULT result = real_->Unacquire();
            if (serviceFfb()) {
                result = DI_OK;
                FfbForwarder::instance().stopAll(this);   // DirectInput stops a device's effects when it is unacquired
            }
            const std::lock_guard<std::mutex> lock(mutex_);
            if (acquired_) {
                acquired_ = false;
                WE_LOG_INFO(detail::emulationLog(), "{}: unacquired", label_);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Poll() override {
        return guarded("Poll", [&]() -> HRESULT {
            HRESULT result = real_->Poll();
            const std::lock_guard<std::mutex> lock(mutex_);
            if (serviceFfb()) {
                result = acquired_ ? DI_NOEFFECT : DIERR_NOTACQUIRED;
            }
            if (acquired_) {
                refresh();
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        return guarded("GetDeviceState", [&]() -> HRESULT {
            ++stateCalls_;
            const std::lock_guard<std::mutex> lock(mutex_);
            if (data == nullptr || !joystick_.hasDataFormat() || size != joystick_.dataSize()) {
                WE_LOG_WARN_LIMITED(detail::emulationLog(), stateFailureLimiter_,
                                    "{}: GetDeviceState({} bytes) with data format of {} bytes -> invalid", label_,
                                                        size,
                                    joystick_.dataSize());
                return DIERR_INVALIDPARAM;
            }
            if (!acquired_) {
                return DIERR_NOTACQUIRED;
            }
            refresh();
            joystick_.writeState(lastReport_, std::span<std::uint8_t>(static_cast<std::uint8_t*>(data), size));
            if (const auto call = stateSampler_.next(); call != 0) {
                WE_LOG_DEBUG(detail::emulationLog(), "{}: GetDeviceState #{} steer={} thr={} brk={} clt={} btn={:08x} "
                                                     "hat={} -> {}",
                             label_, call, lastReport_.steering, lastReport_.throttle, lastReport_.brake,
                             lastReport_.clutch, lastReport_.buttons, lastReport_.hat, describeDeviceState(size, data));
            }
            return DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut,
                                            DWORD flags) override {
        return guarded("GetDeviceData", [&]() -> HRESULT {
            ++dataCalls_;
            if (inOut == nullptr ||
                (objectSize != sizeof(DIDEVICEOBJECTDATA) && objectSize != sizeof(DIDEVICEOBJECTDATA_DX3))) {
                return DIERR_INVALIDPARAM;
            }
            const std::lock_guard<std::mutex> lock(mutex_);
            if (bufferSize_ == 0) {
                return DIERR_NOTBUFFERED;
            }
            if (!acquired_) {
                return DIERR_NOTACQUIRED;
            }
            refresh();

            const bool peek = (flags & DIGDD_PEEK) != 0;
            const std::size_t available = buffer_.size();
            const std::size_t requested = *inOut;   // INFINITE with data == nullptr means "flush everything"
            const std::size_t count = std::min(available, requested);
            if (data != nullptr) {
                auto* out = reinterpret_cast<std::uint8_t*>(data);
                for (std::size_t i = 0; i < count; ++i) {
                    std::memcpy(out + i * objectSize, &buffer_[i], objectSize);
                }
            }
            if (!peek) {
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
            }
            *inOut = static_cast<DWORD>(data == nullptr && peek ? available : count);

            const bool overflowed = overflowed_;
            if (!peek) {
                overflowed_ = false;
            }
            if (const auto call = dataSampler_.next(); call != 0 && count != 0) {
                WE_LOG_DEBUG(detail::emulationLog(), "{}: GetDeviceData #{} returned {} of {} events{}", label_, call,
                             count, available, overflowed ? " (buffer overflowed)" : "");
            }
            return overflowed ? DI_BUFFEROVERFLOW : DI_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD objectSize, LPCDIDEVICEOBJECTDATA data, LPDWORD inOut,
                                             DWORD flags) override {
        return guarded("SendDeviceData", [&]() -> HRESULT {
            return real_->SendDeviceData(objectSize, data, inOut, flags);
        });
    }

    // --- Force feedback: to wheelbri-service (default), or passed to the physical wheel ---------------

    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID type, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out,
                                           LPUNKNOWN outer) override {
        return guarded("CreateEffect", [&]() -> HRESULT {
            if (shimConfig().forceFeedback == FfbMode::Off) {
                WE_LOG_INFO(detail::emulationLog(), "{}: CreateEffect({}) refused (force_feedback = off)", label_,
                            describeGuid(type));
                return DIERR_UNSUPPORTED;
            }
            if (serviceFfb()) {
                return createServiceEffect(type, effect, out, outer);
            }
            const HRESULT result = real_->CreateEffect(type, effect, out, outer);
            // Between the real call and the wrapping nothing may throw, or the game would hold an unwrapped effect.
            guardedVoid("CreateEffect", [&] {
                WE_LOG_INFO(detail::emulationLog(), "{}: CreateEffect({}) {} -> {} (played by the physical wheel)",
                            label_, describeGuid(type),
                            describeEffect(type, effect, effect != nullptr ? DIEP_ALLPARAMS : 0),
                            describeHresult(result));
            });
            if (SUCCEEDED(result) && out != nullptr && *out != nullptr) {
                if (outer != nullptr) {
                    // COM aggregation: the object belongs to the caller's outer object and must not be wrapped.
                    WE_LOG_WARN(detail::emulationLog(), "{}: aggregated effect requested, passing it through unwrapped",
                                label_);
                    return result;
                }
                wrapRealEffect(out, type, label_);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffectsCallback callback, LPVOID ref, DWORD type) override {
        return guarded("EnumEffects", [&]() -> HRESULT {
            if (shimConfig().forceFeedback == FfbMode::Off) {
                return DI_OK;   // no effects to list
            }
            // The physical wheel's list: the service plays through its driver, so the same types are available.
            return real_->EnumEffects(callback, ref, type);
        });
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffectInfo* info, REFGUID guid) override {
        return guarded("GetEffectInfo", [&]() -> HRESULT {
            return real_->GetEffectInfo(info, guid);
        });
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override {
        return guarded("GetForceFeedbackState", [&]() -> HRESULT {
            if (serviceFfb()) {
                if (state == nullptr) {
                    return E_POINTER;
                }
                *state = FfbForwarder::instance().stateFlags();
                return DI_OK;
            }
            return real_->GetForceFeedbackState(state);
        });
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override {
        return guarded("SendForceFeedbackCommand", [&]() -> HRESULT {
            if (serviceFfb()) {
                FfbForwarder& forwarder = FfbForwarder::instance();
                switch (command) {
                    case DISFFC_RESET: forwarder.reset(this); break;
                    case DISFFC_STOPALL: forwarder.stopAll(this); break;
                    case DISFFC_PAUSE: forwarder.setPaused(true); break;
                    case DISFFC_CONTINUE: forwarder.setPaused(false); break;
                    case DISFFC_SETACTUATORSON: forwarder.setActuators(true); break;
                    case DISFFC_SETACTUATORSOFF: forwarder.setActuators(false); break;
                    default: return DIERR_INVALIDPARAM;
                }
                if (const auto call = ffCommandSampler_.next(); call != 0) {
                    WE_LOG_INFO(detail::emulationLog(), "{}: SendForceFeedbackCommand({}) (#{})", label_,
                                describeFfCommand(command), call);
                }
                return DI_OK;
            }
            const HRESULT result = real_->SendForceFeedbackCommand(command);
            if (const auto call = ffCommandSampler_.next(); call != 0 || FAILED(result)) {
                WE_LOG_INFO(detail::emulationLog(), "{}: SendForceFeedbackCommand(0x{:x}) -> {} (#{})", label_, command,
                            describeHresult(result), call);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref,
                                                       DWORD flags) override {
        return guarded("EnumCreatedEffectObjects", [&]() -> HRESULT {
            if (serviceFfb()) {
                WE_LOG_DEBUG(detail::emulationLog(), "{}: EnumCreatedEffectObjects -> none listed", label_);
                return DI_OK;
            }
            return real_->EnumCreatedEffectObjects(callback, ref, flags);
        });
    }

    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override {
        return guarded("Escape", [&]() -> HRESULT {
            // Driver-specific commands meant for a Logitech driver must not reach the rig's own driver. Some games
            // send these every frame (LEDs, rev lights), so the log is sampled.
            if (const auto call = escapeSampler_.next(); call != 0) {
                // The payload shows what the game wants (e.g. rev lights or wheel range): first 32 bytes in hex.
                std::string bytes;
                if (escape != nullptr && escape->lpvInBuffer != nullptr) {
                    const auto* in = static_cast<const unsigned char*>(escape->lpvInBuffer);
                    for (DWORD i = 0; i < escape->cbInBuffer && i < 32; ++i) {
                        char hex[4];
                        std::snprintf(hex, sizeof(hex), "%02x ", in[i]);
                        bytes += hex;
                    }
                }
                WE_LOG_DEBUG(detail::emulationLog(), "{}: Escape #{} (command=0x{:x}, {} bytes in: {}) not forwarded",
                             label_, call, escape != nullptr ? escape->dwCommand : 0,
                             escape != nullptr ? escape->cbInBuffer : 0, bytes);
            }
            return DIERR_UNSUPPORTED;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::String file, LPDIENUMEFFECTSINFILECALLBACK callback,
                                                LPVOID ref, DWORD flags) override {
        return guarded("EnumEffectsInFile", [&]() -> HRESULT {
            return real_->EnumEffectsInFile(file, callback, ref, flags);
        });
    }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::String file, DWORD entries, LPDIFILEEFFECT effects,
                                                DWORD flags) override {
        return guarded("WriteEffectToFile", [&]() -> HRESULT {
            return real_->WriteEffectToFile(file, entries, effects, flags);
        });
    }

private:
    ~EmulatedDevice() = default;

    static std::size_t countButtons(const IDeviceProfile& profile) {
        // All the target's buttons, unnamed gaps included (the G920's 12th): the shifter must sit at its real numbers.
        return profile.buttonCount();
    }

    [[nodiscard]] static bool serviceFfb() { return shimConfig().forceFeedback == FfbMode::Service; }

    static std::string describeFfCommand(const DWORD command) {
        switch (command) {
            case DISFFC_RESET: return "RESET";
            case DISFFC_STOPALL: return "STOPALL";
            case DISFFC_PAUSE: return "PAUSE";
            case DISFFC_CONTINUE: return "CONTINUE";
            case DISFFC_SETACTUATORSON: return "SETACTUATORSON";
            case DISFFC_SETACTUATORSOFF: return "SETACTUATORSOFF";
            default: return fmt::format("0x{:x}", command);
        }
    }

    HRESULT createServiceEffect(REFGUID type, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        const auto effectType = EmulatedEffect::typeFor(type);
        if (!effectType) {
            WE_LOG_WARN(detail::emulationLog(), "{}: CreateEffect({}) not supported", label_, describeGuid(type));
            return DIERR_UNSUPPORTED;
        }
        const auto slot = FfbForwarder::instance().allocate(this);
        if (!slot) {
            return DIERR_DEVICEFULL;
        }
        // From here on the slot is this effect's: anything that fails (allocation, logging) must give it back.
        EmulatedEffect* created = nullptr;
        try {
            DWORD wheelOffset = 0;
            DWORD wheelId = 0;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                wheelOffset = joystick_.reportedOffset(0);   // object 0 is always the wheel (X)
                wheelId = joystick_.objects()[0].typeId;
            }
            created = new EmulatedEffect(type, *effectType, *slot, this, label_, wheelOffset, wheelId);
            HRESULT result = DI_OK;
            if (effect != nullptr) {
                result = created->SetParameters(effect, effect->dwSize == sizeof(DIEFFECT) ? DIEP_ALLPARAMS
                                                                                         : DIEP_ALLPARAMS_DX5);
            }
            WE_LOG_INFO(detail::emulationLog(), "{}: CreateEffect({}) {} -> slot {} for wheelbri-service -> {}",
                        label_, describeGuid(type),
                        describeEffect(type, effect, effect != nullptr ? DIEP_ALLPARAMS : 0), slot->slot,
                        describeHresult(result));
            if (FAILED(result)) {
                created->Release();   // gives the slot back
                return result;
            }
            *out = created;
            return DI_OK;
        } catch (...) {
            if (created != nullptr) {
                created->Release();   // gives the slot back
            } else {
                FfbForwarder::instance().release(*slot);
            }
            throw;   // guarded() turns it into an error for the game and logs it
        }
    }

    // Pulls the newest state and, when buffered, queues what changed. Caller holds mutex_.
    void refresh() {
        if (serviceFfb()) {
            FfbForwarder::instance().tick();
        }
        const TargetReport report = StateFeed::instance().current(*profile_);
        if (bufferSize_ != 0 && joystick_.hasDataFormat()) {
            const DWORD now = GetTickCount();
            for (const auto& change : joystick_.diff(lastReport_, report)) {
                if (buffer_.size() >= bufferSize_) {
                    buffer_.pop_front();
                    overflowed_ = true;
                }
                DIDEVICEOBJECTDATA event{};
                event.dwOfs = change.offset;
                event.dwData = change.data;
                event.dwTimeStamp = now;
                event.dwSequence = ++sequence_;
                event.uAppData = static_cast<UINT_PTR>(-1);
                buffer_.push_back(event);
            }
        }
        tracePedals(report);
        lastReport_ = report;
    }

    // Logs the pedals as the game sees them whenever one crosses a quarter of its travel, so a log shows
    // which axis each physical pedal moves and in which direction. Runs on every read, so the line is only built
    // when a pedal crossed, and sampled (every press crosses several quarters). Caller holds mutex_.
    void tracePedals(const TargetReport& report) {
        std::array<int, 4> buckets{};
        std::size_t pedal = 0;
        forEachPedal(report, [&](const EmulatedObject&, const std::int32_t value, const LogicalRange& range) {
            const double span = static_cast<double>(range.max) - static_cast<double>(range.min);
            buckets[pedal++] = static_cast<int>(std::lround(4.0 * (static_cast<double>(value) - range.min) / (span > 0 ? span : 1.0)));
        });
        if (buckets == pedalBuckets_) {
            return;
        }
        pedalBuckets_ = buckets;
        if (const auto call = pedalSampler_.next(); call != 0) {
            std::string line;
            forEachPedal(report, [&](const EmulatedObject& object, const std::int32_t value, const LogicalRange&) {
                line += fmt::format(" {}({})={}", object.name, axisName(object.guid), value);
            });
            WE_LOG_INFO(detail::emulationLog(), "{}: pedals as the game sees them (#{}):{}", label_, call, line);
        }
    }

    // Calls `visit(object, value, range)` for the first four non-steering axes. Caller holds mutex_.
    template <typename Visit>
    void forEachPedal(const TargetReport& report, Visit&& visit) const {
        std::size_t pedal = 0;
        for (std::size_t i = 0; i < joystick_.objects().size() && pedal < pedalBuckets_.size(); ++i) {
            const EmulatedObject& object = joystick_.objects()[i];
            if (!object.isAxis() || object.control == Control::Steering) {
                continue;
            }
            ++pedal;
            visit(object, static_cast<std::int32_t>(joystick_.valueOf(i, report)), joystick_.range(i));
        }
    }

    // Resolves DIPH_BYOFFSET / DIPH_BYID / DIPH_BYUSAGE. Caller holds mutex_.
    std::optional<std::size_t> lookup(const DWORD object, const DWORD how) const {
        switch (how) {
            case DIPH_BYOFFSET: return joystick_.objectAtOffset(object);
            case DIPH_BYID: return joystick_.objectById(object);
            case DIPH_BYUSAGE: return joystick_.objectByUsage(HIWORD(object), LOWORD(object));
            default: return std::nullopt;
        }
    }

    // Caller holds mutex_ (or the object list is only read).
    typename T::DeviceObjectInstance describe(const std::size_t index) const {
        const EmulatedObject& object = joystick_.objects()[index];
        typename T::DeviceObjectInstance info{};
        info.dwSize = sizeof(info);
        info.guidType = guidOf(object.guid);
        info.dwOfs = joystick_.reportedOffset(index);
        info.dwType = object.typeId;
        info.dwFlags = object.isAxis() ? DIDOI_ASPECTPOSITION : 0;
        if ((object.typeId & DIDFT_FFACTUATOR) != 0) {
            info.dwFlags |= DIDOI_FFACTUATOR;
            info.dwFFMaxForce = DI_FFNOMINALMAX;
            info.dwFFForceResolution = 1;
        }
        copyText(info.tszName, object.name);
        info.wUsagePage = object.usagePage;
        info.wUsage = object.usage;
        return info;
    }

    HRESULT getProperty(REFGUID property, LPDIPROPHEADER header) {
        const std::uintptr_t id = propertyId(property);
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::optional<std::size_t> index =
            header->dwHow == DIPH_DEVICE ? std::nullopt : lookup(header->dwObj, header->dwHow);
        const bool hasObject = index.has_value();
        const bool isAxis = hasObject && joystick_.objects()[*index].isAxis();

        const auto dword = [&](const DWORD value) -> HRESULT {
            if (header->dwSize != sizeof(DIPROPDWORD)) {
                return DIERR_INVALIDPARAM;
            }
            reinterpret_cast<LPDIPROPDWORD>(header)->dwData = value;
            return DI_OK;
        };
        const auto range = [&](const LONG min, const LONG max) -> HRESULT {
            if (header->dwSize != sizeof(DIPROPRANGE)) {
                return DIERR_INVALIDPARAM;
            }
            reinterpret_cast<LPDIPROPRANGE>(header)->lMin = min;
            reinterpret_cast<LPDIPROPRANGE>(header)->lMax = max;
            return DI_OK;
        };
        const auto text = [&](const std::string& value) -> HRESULT {
            if (header->dwSize != sizeof(DIPROPSTRING)) {
                return DIERR_INVALIDPARAM;
            }
            copyText(reinterpret_cast<LPDIPROPSTRING>(header)->wsz, value);
            return DI_OK;
        };
        const auto objectRequired = [&]() -> HRESULT {
            return header->dwHow == DIPH_DEVICE ? DIERR_INVALIDPARAM : DIERR_OBJECTNOTFOUND;
        };

        const DeviceIdentity& identity = profile_->identity();
        switch (id) {
            case prop::kVidPid:
                return dword(MAKELONG(identity.vendorId, identity.productId));
            case prop::kProductName:
            case prop::kInstanceName:
                return text(identity.productName);
            case prop::kTypeName: {
                char name[32];
                std::snprintf(name, sizeof(name), "VID_%04X&PID_%04X", identity.vendorId, identity.productId);
                return text(name);
            }
            case prop::kBufferSize:
                return dword(static_cast<DWORD>(bufferSize_));
            case prop::kAxisMode:
                return dword(DIPROPAXISMODE_ABS);
            case prop::kRange:
                if (!isAxis) {
                    return objectRequired();
                } else {
                    const LogicalRange r = joystick_.range(*index);
                    return range(r.min, r.max);
                }
            case prop::kLogicalRange:
            case prop::kPhysicalRange:
                if (!hasObject) {
                    return objectRequired();
                }
                if (joystick_.objects()[*index].isPov()) {
                    return range(0, 35900);
                }
                return isAxis ? range(0, 65535) : range(0, 1);
            case prop::kDeadZone:
                return isAxis ? dword(deadzone_[*index]) : objectRequired();
            case prop::kSaturation:
                return isAxis ? dword(saturation_[*index]) : objectRequired();
            case prop::kGranularity:
                return hasObject ? dword(1) : objectRequired();
            case prop::kGuidAndPath:
                WE_LOG_INFO(detail::emulationLog(), "{}: game asked for the HID path (DIPROP_GUIDANDPATH); it names "
                                                    "the physical wheel",
                            label_);
                return real_->GetProperty(property, header);
            case prop::kFfGain:
                if (serviceFfb()) {
                    return dword(static_cast<DWORD>(std::lround(FfbForwarder::instance().deviceGain() * 10000.0F)));
                }
                return real_->GetProperty(property, header);
            case prop::kAutoCenter:
                if (serviceFfb()) {
                    return dword(DIPROPAUTOCENTER_OFF);   // the service keeps the wheel's own centring off
                }
                return real_->GetProperty(property, header);
            default:
                // Device-level properties of the physical wheel: JOYSTICKID, USERNAME, ...
                if (hasObject) {
                    return DIERR_UNSUPPORTED;
                }
                return real_->GetProperty(property, header);
        }
    }

    HRESULT setProperty(REFGUID property, LPCDIPROPHEADER header) {
        const std::uintptr_t id = propertyId(property);
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::size_t> axes;   // the axes a device- or object-level setting applies to
        if (header->dwHow == DIPH_DEVICE) {
            for (std::size_t i = 0; i < joystick_.objects().size(); ++i) {
                if (joystick_.objects()[i].isAxis()) {
                    axes.push_back(i);
                }
            }
        } else if (const auto index = lookup(header->dwObj, header->dwHow)) {
            if (joystick_.objects()[*index].isAxis()) {
                axes.push_back(*index);
            }
        } else {
            return DIERR_OBJECTNOTFOUND;
        }

        switch (id) {
            case prop::kRange: {
                if (header->dwSize != sizeof(DIPROPRANGE)) {
                    return DIERR_INVALIDPARAM;
                }
                const auto* r = reinterpret_cast<LPCDIPROPRANGE>(header);
                if (r->lMin >= r->lMax || axes.empty()) {
                    return DIERR_INVALIDPARAM;
                }
                for (const std::size_t axis : axes) {
                    joystick_.setRange(axis, {r->lMin, r->lMax});
                }
                return DI_OK;
            }
            case prop::kDeadZone:
            case prop::kSaturation: {
                if (header->dwSize != sizeof(DIPROPDWORD) || axes.empty()) {
                    return DIERR_INVALIDPARAM;
                }
                const DWORD value = reinterpret_cast<LPCDIPROPDWORD>(header)->dwData;
                if (value > 10000) {
                    return DIERR_INVALIDPARAM;
                }
                auto& store = id == prop::kDeadZone ? deadzone_ : saturation_;
                for (const std::size_t axis : axes) {
                    store[axis] = value;
                }
                // Stored and reported back, not applied: the service already applies dead zones and
                // saturation from its own config, and games rarely set anything but 0 / 10000 here.
                return DI_OK;
            }
            case prop::kBufferSize: {
                if (header->dwSize != sizeof(DIPROPDWORD) || header->dwHow != DIPH_DEVICE) {
                    return DIERR_INVALIDPARAM;
                }
                if (acquired_) {
                    return DIERR_ACQUIRED;
                }
                // Capped: a game that sets a huge buffer and never reads it must not grow it without end.
                bufferSize_ = std::min<std::size_t>(reinterpret_cast<LPCDIPROPDWORD>(header)->dwData, 4096);
                buffer_.clear();
                return DI_OK;
            }
            case prop::kAxisMode:
            case prop::kCalibrationMode:
                return DI_OK;   // absolute axes only; nothing to calibrate
            case prop::kFfGain:
                if (serviceFfb()) {
                    if (header->dwSize != sizeof(DIPROPDWORD) || header->dwHow != DIPH_DEVICE) {
                        return DIERR_INVALIDPARAM;
                    }
                    const DWORD gain = reinterpret_cast<LPCDIPROPDWORD>(header)->dwData;
                    FfbForwarder::instance().setDeviceGain(static_cast<float>(std::min<DWORD>(gain, 10000)) / 10000.0F);
                    return DI_OK;
                }
                return real_->SetProperty(property, header);
            case prop::kAutoCenter:
                if (serviceFfb()) {
                    return DI_OK;   // spring effects centre the wheel; the device's own centring stays off
                }
                return real_->SetProperty(property, header);
            default:
                // Device settings that belong to the physical wheel.
                if (header->dwHow != DIPH_DEVICE) {
                    return DIERR_UNSUPPORTED;
                }
                return real_->SetProperty(property, header);
        }
    }

    void logAssignment() {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < joystick_.objects().size(); ++i) {
            const auto offset = joystick_.formatOffset(i);
            WE_LOG_DEBUG(detail::emulationLog(), "{}:   {} -> {}", label_, joystick_.objects()[i].name,
                         offset ? "offset " + std::to_string(*offset) : std::string("not in the game's format"));
        }
    }

    typename T::Device* real_;
    std::unique_ptr<IDeviceProfile> profile_;
    EmulatedJoystick joystick_;
    std::string label_;
    std::atomic<ULONG> references_{1};

    std::mutex mutex_;
    bool acquired_ = false;
    TargetReport lastReport_;
    std::size_t bufferSize_ = 0;
    std::deque<DIDEVICEOBJECTDATA> buffer_;
    bool overflowed_ = false;
    DWORD sequence_ = 0;
    std::array<int, 4> pedalBuckets_{-1, -1, -1, -1};
    std::array<DWORD, 64> deadzone_{};
    std::array<DWORD, 64> saturation_ = [] {
        std::array<DWORD, 64> values{};
        values.fill(10000);
        return values;
    }();

    std::atomic<std::uint64_t> stateCalls_{0};
    std::atomic<std::uint64_t> dataCalls_{0};
    logging::CallSampler stateSampler_{10, 2000};
    logging::CallSampler dataSampler_{10, 2000};
    logging::CallSampler escapeSampler_{5, 1000};
    logging::CallSampler pedalSampler_{50, 100};
    logging::CallSampler propertySampler_{40, 1000};
    logging::CallSampler setPropertySampler_{40, 1000};
    logging::CallSampler ffCommandSampler_{20, 1000};
    logging::RateLimiter stateFailureLimiter_{std::chrono::seconds(5)};
    logging::RateLimiter acquireLimiter_{std::chrono::seconds(5)};
    logging::RateLimiter physicalAcquireLimiter_{std::chrono::seconds(5)};
};

}  // namespace wheelbri::shim
