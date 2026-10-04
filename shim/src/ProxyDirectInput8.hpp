#pragma once

#include "EmulatedDevice.hpp"
#include "Guard.hpp"
#include "ProxyDevice.hpp"
#include "ShimLogging.hpp"
#include "Traits.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace wheelbri::shim {

namespace detail {

inline const logging::Logger& directInputLog() {
    static const logging::Logger instance = logger("shim.di");
    return instance;
}

}  // namespace detail

// Wraps the game's IDirectInput8 object: logs device enumeration (the step where a game decides whether
// a wheel is "supported") and wraps every created device so its use is logged too. The physical wheel
// named in wheelbri-shim.ini is presented as the emulated wheel (EmulatedDevice); everything else is
// passed through with logging (ProxyDevice).
template <typename T>
class ProxyDirectInput8 final : public T::DirectInput {
public:
    explicit ProxyDirectInput8(typename T::DirectInput* real) : real_(real) {
        WE_LOG_INFO(detail::directInputLog(), "IDirectInput8{} proxy created", T::kSuffix);
    }

    ProxyDirectInput8(const ProxyDirectInput8&) = delete;
    ProxyDirectInput8& operator=(const ProxyDirectInput8&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == T::directInputIid()) {
            *object = this;
            AddRef();
            return S_OK;
        }
        const HRESULT result = real_->QueryInterface(riid, object);
        guardedVoid("QueryInterface", [&] {
            WE_LOG_WARN(detail::directInputLog(), "QueryInterface({}) passed to the real object unwrapped -> {}",
                        describeGuid(riid), describeHresult(result));
        });
        return result;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            const ULONG realRemaining = real_->Release();
            guardedVoid("Release", [&] {
                WE_LOG_INFO(detail::directInputLog(), "IDirectInput8{} released (real object refs left {})",
                            T::kSuffix, realRemaining);
            });
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, typename T::Device** device, LPUNKNOWN outer) override {
        return guarded("CreateDevice", [&]() -> HRESULT {
            const std::string label = labelFor(guid);
            const HRESULT result = real_->CreateDevice(guid, device, outer);
            // Between the real call and the wrapping nothing may throw, or the game would hold an unwrapped device.
            guardedVoid("CreateDevice", [&] {
                WE_LOG_INFO(detail::directInputLog(), "CreateDevice({}) -> {}", label, describeHresult(result));
            });
            if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
                if (outer != nullptr) {
                    // COM aggregation: the object belongs to the caller's outer object and must not be wrapped.
                    WE_LOG_WARN(detail::directInputLog(), "CreateDevice({}): aggregated device requested, passing it "
                                                          "through unwrapped",
                                label);
                    return result;
                }
                // The game gets the wrapper or nothing: if wrapping fails, the real device is released and the
                // exception goes on to guarded().
                typename T::Device* const realDevice = *device;
                *device = nullptr;
                try {
                    // Decide by what the device is, not by how the game found it: a game may reuse an instance
                    // GUID saved from an earlier session without enumerating first.
                    typename T::DeviceInstance info{};
                    info.dwSize = sizeof(info);
                    auto profile = makeProfile(shimConfig().targetProfile);
                    if (profile != nullptr && SUCCEEDED(realDevice->GetDeviceInfo(&info)) && isSourceDevice(info)) {
                        *device = new EmulatedDevice<T>(realDevice, std::move(profile), toUtf8(info.tszProductName));
                    } else {
                        *device = new ProxyDevice<T>(realDevice, label);
                    }
                } catch (...) {
                    realDevice->Release();
                    throw;
                }
            }
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD devType, typename T::EnumDevicesCallback callback, LPVOID ref,
                                          DWORD flags) override {
        return guarded("EnumDevices", [&]() -> HRESULT {
            if (callback == nullptr) {   // the real DLL gets our callback, so it cannot see the game's null
                return DIERR_INVALIDPARAM;
            }
            WE_LOG_INFO(detail::directInputLog(), "EnumDevices(filter={}, flags={})", describeDevType(devType),
                        describeEnumFlags(flags));
            EnumContext context{callback, ref, this,
                                shimConfig().emulate ? makeProfile(shimConfig().targetProfile) : nullptr};
            // A game asking only for driving wheels would never be offered a wheel that DirectInput classes
            // differently (a wheel may report as a joystick). Ask for all game controllers and filter here.
            DWORD realFilter = devType;
            if (context.profile != nullptr && devType == DI8DEVTYPE_DRIVING) {
                realFilter = DI8DEVCLASS_GAMECTRL;
                context.onlyDriving = true;
            }
            const HRESULT result = real_->EnumDevices(realFilter, &ProxyDirectInput8::onDevice, &context, flags);
            WE_LOG_INFO(detail::directInputLog(), "EnumDevices -> {} ({} devices offered, game accepted {})",
                        describeHresult(result), context.offered, context.accepted);
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override {
        return guarded("GetDeviceStatus", [&]() -> HRESULT {
            const HRESULT result = real_->GetDeviceStatus(guid);
            WE_LOG_DEBUG(detail::directInputLog(), "GetDeviceStatus({}) -> {}", labelFor(guid),
                         describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND owner, DWORD flags) override {
        return guarded("RunControlPanel", [&]() -> HRESULT {
            const HRESULT result = real_->RunControlPanel(owner, flags);
            WE_LOG_INFO(detail::directInputLog(), "RunControlPanel -> {}", describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version) override {
        return guarded("Initialize", [&]() -> HRESULT {
            const HRESULT result = real_->Initialize(instance, version);
            WE_LOG_INFO(detail::directInputLog(), "Initialize(version 0x{:x}) -> {}", version, describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guidClass, typename T::String name, LPGUID instance) override {
        return guarded("FindDevice", [&]() -> HRESULT {
            const HRESULT result = real_->FindDevice(guidClass, name, instance);
            WE_LOG_INFO(detail::directInputLog(), "FindDevice(class={}, name='{}') -> {}{}", describeGuid(guidClass),
                        toUtf8(name), describeHresult(result),
                        SUCCEEDED(result) && instance != nullptr ? " instance=" + guidToString(*instance) : "");
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::String user, typename T::ActionFormat format,
                                                     typename T::EnumBySemanticsCallback callback, LPVOID ref,
                                                     DWORD flags) override {
        return guarded("EnumDevicesBySemantics", [&]() -> HRESULT {
            // Devices handed out here are real, unwrapped objects. Logged loudly: if a game uses this path,
            // Phase 2 must wrap it too.
            const HRESULT result = real_->EnumDevicesBySemantics(user, format, callback, ref, flags);
            WE_LOG_WARN(detail::directInputLog(),
                        "EnumDevicesBySemantics(user='{}', flags=0x{:x}) -> {}: devices from this call are NOT proxied",
                        toUtf8(user), flags, describeHresult(result));
            return result;
        });
    }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK callback, typename T::ConfigureParams params,
                                               DWORD flags, LPVOID ref) override {
        return guarded("ConfigureDevices", [&]() -> HRESULT {
            const HRESULT result = real_->ConfigureDevices(callback, params, flags, ref);
            WE_LOG_INFO(detail::directInputLog(), "ConfigureDevices(flags=0x{:x}) -> {}", flags,
                        describeHresult(result));
            return result;
        });
    }

private:
    struct EnumContext {
        typename T::EnumDevicesCallback callback;
        LPVOID ref;
        ProxyDirectInput8* self;
        std::unique_ptr<IDeviceProfile> profile;   // identity the physical wheel is presented with (or null)
        bool onlyDriving = false;                   // filter to DI8DEVTYPE_DRIVING after presenting
        std::size_t offered = 0;
        std::size_t accepted = 0;
    };

    ~ProxyDirectInput8() = default;

    static BOOL CALLBACK onDevice(const typename T::DeviceInstance* instance, LPVOID ref) {
        // Called by the real DLL: an exception here would unwind through its frames.
        return guardedOr<BOOL>(DIENUM_STOP, "onDevice", [&]() -> BOOL {
            auto* const context = static_cast<EnumContext*>(ref);
            ++context->offered;
            if (instance != nullptr) {
                context->self->remember(*instance);
            }
            // The physical wheel is offered to the game as the emulated one. Copy only what the caller's
            // (possibly shorter, DirectX 3) struct holds.
            typename T::DeviceInstance presented{};
            const typename T::DeviceInstance* offered = instance;
            if (instance != nullptr && context->profile != nullptr && isSourceDevice(*instance)) {
                std::memcpy(&presented, instance, std::min<std::size_t>(instance->dwSize, sizeof(presented)));
                presentAs(presented, context->profile->identity());
                offered = &presented;
                WE_LOG_INFO(detail::directInputLog(), "  presenting '{}' {} to the game as '{}' {}",
                            toUtf8(instance->tszProductName), describeGuid(instance->guidProduct),
                            toUtf8(presented.tszProductName), describeGuid(presented.guidProduct));
            }
            if (context->onlyDriving && offered != nullptr &&
                GET_DIDEVICE_TYPE(offered->dwDevType) != DI8DEVTYPE_DRIVING) {
                return DIENUM_CONTINUE;
            }
            const BOOL next = context->callback(offered, context->ref);
            if (next == DIENUM_STOP) {
                ++context->accepted;  // most games stop enumerating at the device they chose
            }
            if (instance != nullptr) {
                WE_LOG_INFO(detail::directInputLog(),
                            "  device '{}' instance={} product={} type={} ffDriver={} usage={:x}/{:x} -> game {}",
                            toUtf8(instance->tszProductName), guidToString(instance->guidInstance),
                            describeGuid(instance->guidProduct), describeDevType(instance->dwDevType),
                            guidToString(instance->guidFFDriver), instance->wUsagePage, instance->wUsage,
                            next == DIENUM_STOP ? "STOPPED here (picked this one?)" : "continued");
            }
            return next;
        });
    }

    void remember(const typename T::DeviceInstance& instance) {
        const std::lock_guard<std::mutex> lock(namesMutex_);
        names_[guidToString(instance.guidInstance)] = toUtf8(instance.tszProductName);
    }

    std::string labelFor(REFGUID guid) {
        const std::string key = guidToString(guid);
        const std::lock_guard<std::mutex> lock(namesMutex_);
        const auto it = names_.find(key);
        return it != names_.end() ? "[" + it->second + " " + key + "]" : "[" + describeGuid(guid) + "]";
    }

    typename T::DirectInput* real_;
    std::atomic<ULONG> references_{1};
    std::mutex namesMutex_;
    std::map<std::string, std::string> names_;  // instance GUID -> product name, from EnumDevices
};

}  // namespace wheelbri::shim
