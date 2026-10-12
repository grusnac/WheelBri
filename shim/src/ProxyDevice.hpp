#pragma once

#include "DeviceAudit.hpp"
#include "ProxyEffect.hpp"
#include "Guard.hpp"
#include "ShimLogging.hpp"
#include "Traits.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace wheelbri::shim {

namespace detail {

inline const logging::Logger& deviceLog() {
    static const logging::Logger instance = logger("shim.device");
    return instance;
}

inline const logging::Logger& stateLog() {
    static const logging::Logger instance = logger("shim.state");
    return instance;
}

}  // namespace detail

// Phase 0: logs every call the game makes on a device (what it queries, how it reads, which
// force feedback it creates) and forwards it unchanged. Phase 2 replaces the answers for the wheel.
template <typename T>
class ProxyDevice final : public T::Device {
public:
    ProxyDevice(typename T::Device* real, std::string label) : real_(real), label_(std::move(label)), audit_(label_) {
        WE_LOG_INFO(detail::deviceLog(), "{}: device proxy created ({})", label_, T::kSuffix);
    }

    ProxyDevice(const ProxyDevice&) = delete;
    ProxyDevice& operator=(const ProxyDevice&) = delete;

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
            WE_LOG_WARN(detail::deviceLog(), "{}: QueryInterface({}) passed to the real device unwrapped -> {}",
                        label_, describeGuid(riid), describeHresult(result));
        });
        return result;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            const ULONG realRemaining = real_->Release();
            guardedVoid("Release", [&] {
                audit_.summary("Release");
                WE_LOG_INFO(detail::deviceLog(), "{}: released after {} GetDeviceState / {} GetDeviceData / {} Poll "
                                                 "calls (real object refs left {})",
                            label_, stateCalls_.load(), dataCalls_.load(), pollCalls_.load(), realRemaining);
            });
            delete this;
        }
        return remaining;
    }

    // --- Device description: what the game learns about the device -----------------------------

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        return guarded("GetCapabilities", [&]() -> HRESULT {
            const HRESULT result = real_->GetCapabilities(caps);
            audit_.call("GetCapabilities");
            WE_LOG_INFO(detail::deviceLog(), "{}: GetCapabilities -> {} {}", label_, describeHresult(result),
                        SUCCEEDED(result) && caps != nullptr ? describeCaps(*caps) : std::string());
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjectsCallback callback, LPVOID ref, DWORD flags) override {
        return guarded("EnumObjects", [&]() -> HRESULT {
            if (callback == nullptr) {   // the real DLL gets our callback, so it cannot see the game's null
                return DIERR_INVALIDPARAM;
            }
            WE_LOG_INFO(detail::deviceLog(), "{}: EnumObjects(flags=0x{:x})", label_, flags);
            audit_.call(flagsKind("EnumObjects", flags));
            EnumContext<typename T::EnumObjectsCallback> context{callback, ref, this};
            const HRESULT result = real_->EnumObjects(&ProxyDevice::onObject, &context, flags);
            WE_LOG_INFO(detail::deviceLog(), "{}: EnumObjects -> {} ({} objects)", label_, describeHresult(result),
                        context.count);
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER header) override {
        return guarded("GetProperty", [&]() -> HRESULT {
            const HRESULT result = real_->GetProperty(property, header);
            audit_.getProperty(property, header, result);
            if (const auto call = propertySampler_.next(); call != 0) {   // flushed lines: sampled
                WE_LOG_INFO(detail::deviceLog(), "{}: GetProperty({}) -> {} {} (#{})", label_, describeProperty(property),
                            describeHresult(result),
                            SUCCEEDED(result) ? describePropertyValue(property, header) : std::string(), call);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER header) override {
        return guarded("SetProperty", [&]() -> HRESULT {
            const HRESULT result = real_->SetProperty(property, header);
            audit_.setProperty(property, header, result);
            if (const auto call = setPropertySampler_.next(); call != 0) {
                WE_LOG_INFO(detail::deviceLog(), "{}: SetProperty({}, {}) -> {} (#{})", label_, describeProperty(property),
                            describePropertyValue(property, header), describeHresult(result), call);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::DeviceObjectInstance* info, DWORD object, DWORD how) override {
        return guarded("GetObjectInfo", [&]() -> HRESULT {
            const HRESULT result = real_->GetObjectInfo(info, object, how);
            audit_.call("GetObjectInfo", SUCCEEDED(result) && info != nullptr ? describeObject(*info) : describeHresult(result));
            WE_LOG_DEBUG(detail::deviceLog(), "{}: GetObjectInfo(obj=0x{:x}, how={}) -> {}{}", label_, object, how,
                         describeHresult(result),
                         SUCCEEDED(result) && info != nullptr ? " " + describeObject(*info) : "");
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DeviceInstance* info) override {
        return guarded("GetDeviceInfo", [&]() -> HRESULT {
            const HRESULT result = real_->GetDeviceInfo(info);
            audit_.call("GetDeviceInfo");
            WE_LOG_INFO(detail::deviceLog(), "{}: GetDeviceInfo -> {}{}", label_, describeHresult(result),
                        SUCCEEDED(result) && info != nullptr ? " " + describeInstance(*info) : "");
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImageInfoHeader header) override {
        return guarded("GetImageInfo", [&]() -> HRESULT {
            const HRESULT result = real_->GetImageInfo(header);
            WE_LOG_DEBUG(detail::deviceLog(), "{}: GetImageInfo -> {}", label_, describeHresult(result));
            return result;
        });
    }

    // --- Setup ------------------------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override {
        return guarded("SetDataFormat", [&]() -> HRESULT {
            const HRESULT result = real_->SetDataFormat(format);
            audit_.call("SetDataFormat", describeDataFormat(format));
            WE_LOG_INFO(detail::deviceLog(), "{}: SetDataFormat({}) -> {}", label_, describeDataFormat(format),
                        describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override {
        return guarded("SetCooperativeLevel", [&]() -> HRESULT {
            const HRESULT result = real_->SetCooperativeLevel(window, flags);
            audit_.call("SetCooperativeLevel");
            WE_LOG_INFO(detail::deviceLog(), "{}: SetCooperativeLevel(hwnd={}, {}) -> {}", label_,
                        static_cast<void*>(window), describeCooperativeLevel(flags), describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override {
        return guarded("SetEventNotification", [&]() -> HRESULT {
            const HRESULT result = real_->SetEventNotification(event);
            WE_LOG_DEBUG(detail::deviceLog(), "{}: SetEventNotification({}) -> {}", label_, static_cast<void*>(event),
                         describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override {
        return guarded("Initialize", [&]() -> HRESULT {
            const HRESULT result = real_->Initialize(instance, version, guid);
            WE_LOG_DEBUG(detail::deviceLog(), "{}: Initialize(version 0x{:x}, {}) -> {}", label_, version,
                         describeGuid(guid), describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND owner, DWORD flags) override {
        return guarded("RunControlPanel", [&]() -> HRESULT {
            const HRESULT result = real_->RunControlPanel(owner, flags);
            audit_.risky("RunControlPanel", describeHresult(result));
            WE_LOG_INFO(detail::deviceLog(), "{}: RunControlPanel -> {}", label_, describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActionFormat format, typename T::String user,
                                             DWORD flags) override {
        return guarded("BuildActionMap", [&]() -> HRESULT {
            const HRESULT result = real_->BuildActionMap(format, user, flags);
            audit_.risky("BuildActionMap", "user '" + toUtf8(user) + "' -> " + describeHresult(result));
            WE_LOG_INFO(detail::deviceLog(), "{}: BuildActionMap(user='{}', flags=0x{:x}) -> {}", label_, toUtf8(user),
                        flags, describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActionFormat format, typename T::String user,
                                           DWORD flags) override {
        return guarded("SetActionMap", [&]() -> HRESULT {
            const HRESULT result = real_->SetActionMap(format, user, flags);
            audit_.risky("SetActionMap", "user '" + toUtf8(user) + "' -> " + describeHresult(result));
            WE_LOG_INFO(detail::deviceLog(), "{}: SetActionMap(user='{}', flags=0x{:x}) -> {}", label_, toUtf8(user),
                        flags, describeHresult(result));
            return result;
        });
    }

    // --- Acquisition and reading: the hot path ------------------------------------------------------

    HRESULT STDMETHODCALLTYPE Acquire() override {
        return guarded("Acquire", [&]() -> HRESULT {
            const HRESULT result = real_->Acquire();
            logStateful("Acquire", result);
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Unacquire() override {
        return guarded("Unacquire", [&]() -> HRESULT {
            const HRESULT result = real_->Unacquire();
            logStateful("Unacquire", result);
            if (unacquireSummaries_.next() != 0) audit_.summary("Unacquire");
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        return guarded("GetDeviceState", [&]() -> HRESULT {
            const HRESULT result = real_->GetDeviceState(size, data);
            ++stateCalls_;
            audit_.tick();
            if (FAILED(result)) {
                WE_LOG_WARN_LIMITED(detail::stateLog(), stateFailureLimiter_, "{}: GetDeviceState({} bytes) -> {}",
                                    label_,
                                    size, describeHresult(result));
            } else if (const auto call = stateSampler_.next(); call != 0) {
                WE_LOG_DEBUG(detail::stateLog(), "{}: GetDeviceState #{} ({} bytes) {}", label_, call, size,
                             describeDeviceState(size, data));
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD objectSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut,
                                            DWORD flags) override {
        return guarded("GetDeviceData", [&]() -> HRESULT {
            const DWORD requested = inOut != nullptr ? *inOut : 0;
            const HRESULT result = real_->GetDeviceData(objectSize, data, inOut, flags);
            ++dataCalls_;
            audit_.tick();
            if (FAILED(result)) {
                WE_LOG_WARN_LIMITED(detail::stateLog(), dataFailureLimiter_, "{}: GetDeviceData -> {}", label_,
                                    describeHresult(result));
            } else if (const auto call = dataSampler_.next(); call != 0) {
                WE_LOG_DEBUG(detail::stateLog(), "{}: GetDeviceData #{} requested={} returned={} flags=0x{:x} -> {}",
                             label_, call, requested, inOut != nullptr ? *inOut : 0, flags, describeHresult(result));
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Poll() override {
        return guarded("Poll", [&]() -> HRESULT {
            const HRESULT result = real_->Poll();
            ++pollCalls_;
            if (FAILED(result)) {
                WE_LOG_WARN_LIMITED(detail::stateLog(), pollFailureLimiter_, "{}: Poll -> {}", label_,
                                    describeHresult(result));
            } else if (const auto call = pollSampler_.next(); call != 0) {
                WE_LOG_TRACE(detail::stateLog(), "{}: Poll #{} -> {}", label_, call, describeHresult(result));
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD objectSize, LPCDIDEVICEOBJECTDATA data, LPDWORD inOut,
                                             DWORD flags) override {
        return guarded("SendDeviceData", [&]() -> HRESULT {
            const HRESULT result = real_->SendDeviceData(objectSize, data, inOut, flags);
            audit_.risky("SendDeviceData", describeHresult(result));
            WE_LOG_INFO(detail::deviceLog(), "{}: SendDeviceData(count={}, flags=0x{:x}) -> {}", label_,
                        inOut != nullptr ? *inOut : 0, flags, describeHresult(result));
            return result;
        });
    }

    // --- Force feedback -----------------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID type, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out,
                                           LPUNKNOWN outer) override {
        return guarded("CreateEffect", [&]() -> HRESULT {
            const HRESULT result = real_->CreateEffect(type, effect, out, outer);
            // Between the real call and the wrapping nothing may throw, or the game would hold an unwrapped effect.
            guardedVoid("CreateEffect", [&] {
                WE_LOG_INFO(detail::deviceLog(), "{}: CreateEffect({}) {} -> {}", label_, describeGuid(type),
                            describeEffect(type, effect, effect != nullptr ? DIEP_ALLPARAMS : 0),
                            describeHresult(result));
            });
            if (SUCCEEDED(result) && out != nullptr && *out != nullptr) {
                if (outer != nullptr) {
                    // COM aggregation: the object belongs to the caller's outer object and must not be wrapped.
                    WE_LOG_WARN(detail::deviceLog(), "{}: aggregated effect requested, passing it through unwrapped",
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
            if (callback == nullptr) {
                return DIERR_INVALIDPARAM;
            }
            WE_LOG_INFO(detail::deviceLog(), "{}: EnumEffects(type=0x{:x})", label_, type);
            EnumContext<typename T::EnumEffectsCallback> context{callback, ref, this};
            const HRESULT result = real_->EnumEffects(&ProxyDevice::onEffect, &context, type);
            WE_LOG_INFO(detail::deviceLog(), "{}: EnumEffects -> {} ({} effects)", label_, describeHresult(result),
                        context.count);
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffectInfo* info, REFGUID guid) override {
        return guarded("GetEffectInfo", [&]() -> HRESULT {
            const HRESULT result = real_->GetEffectInfo(info, guid);
            WE_LOG_DEBUG(detail::deviceLog(), "{}: GetEffectInfo({}) -> {}", label_, describeGuid(guid),
                         describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override {
        return guarded("GetForceFeedbackState", [&]() -> HRESULT {
            const HRESULT result = real_->GetForceFeedbackState(state);
            if (const auto call = ffStateSampler_.next(); call != 0) {
                WE_LOG_DEBUG(detail::deviceLog(), "{}: GetForceFeedbackState #{} -> {} state=0x{:x}", label_, call,
                             describeHresult(result), state != nullptr ? *state : 0);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override {
        return guarded("SendForceFeedbackCommand", [&]() -> HRESULT {
            const HRESULT result = real_->SendForceFeedbackCommand(command);
            if (const auto call = ffCommandSampler_.next(); call != 0) {
                WE_LOG_INFO(detail::deviceLog(), "{}: SendForceFeedbackCommand({}) -> {} (#{})", label_,
                            describeFfCommand(command), describeHresult(result), call);
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref,
                                                       DWORD flags) override {
        return guarded("EnumCreatedEffectObjects", [&]() -> HRESULT {
            // Callers receive the real effect objects here; logged so Phase 3 knows whether this matters.
            const HRESULT result = real_->EnumCreatedEffectObjects(callback, ref, flags);
            WE_LOG_WARN(detail::deviceLog(), "{}: EnumCreatedEffectObjects -> {} (effects handed out unwrapped)",
                        label_,
                        describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override {
        return guarded("Escape", [&]() -> HRESULT {
            const HRESULT result = real_->Escape(escape);
            audit_.escape(escape, result);   // every new kind of command in full
            if (const auto call = escapeSampler_.next(); call != 0) {   // some games send one every frame
                const auto* const in = escape != nullptr ? static_cast<const std::uint8_t*>(escape->lpvInBuffer) : nullptr;
                const DWORD size = in != nullptr ? escape->cbInBuffer : 0;
                WE_LOG_INFO(detail::deviceLog(), "{}: Escape #{} (command=0x{:x}, in={} bytes: {}; out={} bytes) -> {}",
                            label_, call, escape != nullptr ? escape->dwCommand : 0, size,
                            hexBytes(std::span<const std::uint8_t>(in, size)),
                            escape != nullptr ? escape->cbOutBuffer : 0, describeHresult(result));
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::String file, LPDIENUMEFFECTSINFILECALLBACK callback,
                                                LPVOID ref, DWORD flags) override {
        return guarded("EnumEffectsInFile", [&]() -> HRESULT {
            const HRESULT result = real_->EnumEffectsInFile(file, callback, ref, flags);
            WE_LOG_INFO(detail::deviceLog(), "{}: EnumEffectsInFile('{}') -> {}", label_, toUtf8(file),
                        describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::String file, DWORD entries, LPDIFILEEFFECT effects,
                                                DWORD flags) override {
        return guarded("WriteEffectToFile", [&]() -> HRESULT {
            const HRESULT result = real_->WriteEffectToFile(file, entries, effects, flags);
            WE_LOG_INFO(detail::deviceLog(), "{}: WriteEffectToFile('{}', {} entries) -> {}", label_, toUtf8(file),
                        entries, describeHresult(result));
            return result;
        });
    }

private:
    template <typename Callback>
    struct EnumContext {
        Callback callback;
        LPVOID ref;
        ProxyDevice* self;
        std::size_t count = 0;
    };

    ~ProxyDevice() = default;

    static std::string describeObject(const typename T::DeviceObjectInstance& object) {
        return describeGuid(object.guidType) + " '" + toUtf8(object.tszName) + "' ofs=" + std::to_string(object.dwOfs) +
               " type=" + describeObjectType(object.dwType) + " flags=0x" + toHex(object.dwFlags) +
               (object.dwSize >= sizeof(typename T::DeviceObjectInstance)
                    ? " usage=" + toHex(object.wUsagePage) + "/" + toHex(object.wUsage)
                    : std::string());
    }

    static std::string describeInstance(const typename T::DeviceInstance& instance) {
        // A DirectX 3 DIDEVICEINSTANCE ends after the product name: guidFFDriver and the usage are not there.
        return "'" + toUtf8(instance.tszProductName) + "' instance='" + toUtf8(instance.tszInstanceName) +
               "' product=" + describeGuid(instance.guidProduct) + " devType=" + describeDevType(instance.dwDevType) +
               (instance.dwSize >= sizeof(typename T::DeviceInstance)
                    ? " ffDriver=" + guidToString(instance.guidFFDriver) + " usage=" + toHex(instance.wUsagePage) +
                          "/" + toHex(instance.wUsage)
                    : std::string());
    }

    static std::string describeFfCommand(const DWORD command) {
        switch (command) {
            case DISFFC_RESET: return "RESET";
            case DISFFC_STOPALL: return "STOPALL";
            case DISFFC_PAUSE: return "PAUSE";
            case DISFFC_CONTINUE: return "CONTINUE";
            case DISFFC_SETACTUATORSON: return "SETACTUATORSON";
            case DISFFC_SETACTUATORSOFF: return "SETACTUATORSOFF";
            default: return "0x" + toHex(command);
        }
    }

    static std::string toHex(const unsigned long value) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%lx", value);
        return buffer;
    }

    static BOOL CALLBACK onObject(const typename T::DeviceObjectInstance* object, LPVOID ref) {
        // Called by the real DLL: an exception here would unwind through its frames.
        return guardedOr<BOOL>(DIENUM_STOP, "onObject", [&]() -> BOOL {
            auto* const context = static_cast<EnumContext<typename T::EnumObjectsCallback>*>(ref);
            ++context->count;
            const BOOL next = context->callback(object, context->ref);
            WE_LOG_DEBUG(detail::deviceLog(), "{}:   object {}{}", context->self->label_,
                         object != nullptr ? describeObject(*object) : std::string("null"),
                         next == DIENUM_STOP ? " (game stopped the enumeration)" : "");
            return next;
        });
    }

    static BOOL CALLBACK onEffect(const typename T::EffectInfo* info, LPVOID ref) {
        // Called by the real DLL: an exception here would unwind through its frames.
        return guardedOr<BOOL>(DIENUM_STOP, "onEffect", [&]() -> BOOL {
            auto* const context = static_cast<EnumContext<typename T::EnumEffectsCallback>*>(ref);
            ++context->count;
            const BOOL next = context->callback(info, context->ref);
            if (info != nullptr) {
                WE_LOG_DEBUG(detail::deviceLog(), "{}:   effect {} '{}' type=0x{:x} static=0x{:x} dynamic=0x{:x}{}",
                             context->self->label_, describeGuid(info->guid), toUtf8(info->tszName), info->dwEffType,
                             info->dwStaticParams, info->dwDynamicParams,
                             next == DIENUM_STOP ? " (game stopped the enumeration)" : "");
            }
            return next;
        });
    }

    void logStateful(const char* call, const HRESULT result) {
        // Games often re-Acquire every frame (S_FALSE = already acquired) or retry after focus loss
        // (DIERR_OTHERAPPHASPRIO); keep those from flooding the log, but always show the first success.
        std::uint64_t suppressed = 0;
        if (result == DI_OK) {
            if (const auto number = acquireOkSampler_.next(); number != 0) {
                WE_LOG_INFO(detail::deviceLog(), "{}: {} #{} -> {}", label_, call, number, describeHresult(result));
            }
        } else if (FAILED(result)) {
            WE_LOG_WARN_LIMITED(detail::deviceLog(), acquireFailureLimiter_, "{}: {} -> {}", label_, call,
                                describeHresult(result));
        } else if (acquireLimiter_.allow(suppressed)) {
            WE_LOG_DEBUG(detail::deviceLog(), "{}: {} -> {} ({} similar suppressed)", label_, call,
                         describeHresult(result), suppressed);
        }
    }

    static std::string flagsKind(const char* call, const DWORD flags) {
        char kind[48];
        std::snprintf(kind, sizeof(kind), "%s flags=0x%lx", call, static_cast<unsigned long>(flags));
        return kind;
    }

    typename T::Device* real_;
    std::string label_;
    DeviceAudit audit_;
    logging::CallSampler escapeSampler_{20, 1000};
    logging::CallSampler unacquireSummaries_{10, 100};
    std::atomic<ULONG> references_{1};
    std::atomic<std::uint64_t> stateCalls_{0};
    std::atomic<std::uint64_t> dataCalls_{0};
    std::atomic<std::uint64_t> pollCalls_{0};
    logging::CallSampler stateSampler_{10, 2000};
    logging::CallSampler dataSampler_{10, 2000};
    logging::CallSampler pollSampler_{5, 5000};
    logging::CallSampler ffStateSampler_{5, 1000};
    logging::CallSampler acquireOkSampler_{20, 1000};
    logging::CallSampler propertySampler_{40, 1000};
    logging::CallSampler setPropertySampler_{40, 1000};
    logging::CallSampler ffCommandSampler_{20, 1000};   // Acquire / Unacquire -> DI_OK, which some games do every frame
    logging::RateLimiter stateFailureLimiter_{std::chrono::seconds(5)};
    logging::RateLimiter dataFailureLimiter_{std::chrono::seconds(5)};
    logging::RateLimiter pollFailureLimiter_{std::chrono::seconds(5)};
    logging::RateLimiter acquireLimiter_{std::chrono::seconds(5)};
    logging::RateLimiter acquireFailureLimiter_{std::chrono::seconds(5)};
};

}  // namespace wheelbri::shim
