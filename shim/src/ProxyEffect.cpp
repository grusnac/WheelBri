#include "ProxyEffect.hpp"
#include "Guard.hpp"

#include <utility>

namespace wheelbri::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.effect");
    return instance;
}

}  // namespace

void wrapRealEffect(LPDIRECTINPUTEFFECT* const out, const GUID& type, const std::string& deviceLabel) {
    IDirectInputEffect* const real = *out;
    *out = nullptr;
    try {
        *out = new ProxyEffect(real, type, deviceLabel);
    } catch (...) {
        real->Release();
        throw;
    }
}

ProxyEffect::ProxyEffect(IDirectInputEffect* const real, const GUID& type, std::string deviceLabel)
    : real_(real), type_(type), label_(std::move(deviceLabel) + " effect " + describeGuid(type)) {
    WE_LOG_DEBUG(log(), "{}: proxy created", label_);
}

HRESULT ProxyEffect::QueryInterface(REFIID riid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDirectInputEffect) {
        *object = this;
        AddRef();
        return S_OK;
    }
    const HRESULT result = real_->QueryInterface(riid, object);
    guardedVoid("QueryInterface", [&] {
        WE_LOG_WARN(log(), "{}: QueryInterface({}) passed to the real object unwrapped -> {}", label_,
                    describeGuid(riid), describeHresult(result));
    });
    return result;
}

ULONG ProxyEffect::AddRef() { return ++references_; }

ULONG ProxyEffect::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) {
        const ULONG realRemaining = real_->Release();
        guardedVoid("Release", [&] {
            WE_LOG_DEBUG(log(), "{}: released (real object refs left {})", label_, realRemaining);
        });
        delete this;
    }
    return remaining;
}

HRESULT ProxyEffect::Initialize(HINSTANCE instance, DWORD version, REFGUID guid) {
    return guarded("Initialize", [&]() -> HRESULT {
        const HRESULT result = real_->Initialize(instance, version, guid);
        WE_LOG_DEBUG(log(), "{}: Initialize(version 0x{:x}) -> {}", label_, version, describeHresult(result));
        return result;
    });
}

HRESULT ProxyEffect::GetEffectGuid(LPGUID guid) {
    return guarded("GetEffectGuid", [&]() -> HRESULT { return real_->GetEffectGuid(guid); });
}

HRESULT ProxyEffect::GetParameters(LPDIEFFECT effect, DWORD flags) {
    return guarded("GetParameters", [&]() -> HRESULT {
        const HRESULT result = real_->GetParameters(effect, flags);
        WE_LOG_TRACE(log(), "{}: GetParameters -> {} {}", label_, describeHresult(result),
                     SUCCEEDED(result) ? describeEffect(type_, effect, flags) : std::string());
        return result;
    });
}

HRESULT ProxyEffect::SetParameters(LPCDIEFFECT effect, DWORD flags) {
    return guarded("SetParameters", [&]() -> HRESULT {
        const HRESULT result = real_->SetParameters(effect, flags);
        // Games update constant forces every frame: log the first calls in full, then sample. Failures always.
        if (FAILED(result)) {
            static logging::RateLimiter limiter(std::chrono::seconds(5));
            WE_LOG_WARN_LIMITED(log(), limiter, "{}: SetParameters {} -> {}", label_, describeEffect(type_, effect,
                                flags),
                                describeHresult(result));
        } else if (const auto call = setParametersSampler_.next(); call != 0) {
            WE_LOG_DEBUG(log(), "{}: SetParameters #{} {} -> {}", label_, call, describeEffect(type_, effect, flags),
                         describeHresult(result));
        }
        return result;
    });
}

HRESULT ProxyEffect::Start(DWORD iterations, DWORD flags) {
    return guarded("Start", [&]() -> HRESULT {
        const HRESULT result = real_->Start(iterations, flags);
        static logging::CallSampler sampler(20, 500);   // may come every frame
        if (const auto call = sampler.next(); call != 0) {
            WE_LOG_DEBUG(log(), "{}: Start #{} (iterations={}, flags=0x{:x}{}{}) -> {}", label_, call,
                         iterations == INFINITE ? std::string("INFINITE") : std::to_string(iterations), flags,
                         (flags & DIES_SOLO) != 0 ? " SOLO" : "", (flags & DIES_NODOWNLOAD) != 0 ? " NODOWNLOAD" : "",
                         describeHresult(result));
        }
        return result;
    });
}

HRESULT ProxyEffect::Stop() {
    return guarded("Stop", [&]() -> HRESULT {
        const HRESULT result = real_->Stop();
        static logging::CallSampler sampler(20, 500);
        if (const auto call = sampler.next(); call != 0) {
            WE_LOG_DEBUG(log(), "{}: Stop #{} -> {}", label_, call, describeHresult(result));
        }
        return result;
    });
}

HRESULT ProxyEffect::GetEffectStatus(LPDWORD status) {
    return guarded("GetEffectStatus", [&]() -> HRESULT {
        const HRESULT result = real_->GetEffectStatus(status);
        if (const auto call = statusSampler_.next(); call != 0) {
            WE_LOG_TRACE(log(), "{}: GetEffectStatus #{} -> {} status=0x{:x}", label_, call, describeHresult(result),
                         status != nullptr ? *status : 0);
        }
        return result;
    });
}

HRESULT ProxyEffect::Download() {
    return guarded("Download", [&]() -> HRESULT {
        const HRESULT result = real_->Download();
        WE_LOG_DEBUG(log(), "{}: Download -> {}", label_, describeHresult(result));
        return result;
    });
}

HRESULT ProxyEffect::Unload() {
    return guarded("Unload", [&]() -> HRESULT {
        const HRESULT result = real_->Unload();
        WE_LOG_DEBUG(log(), "{}: Unload -> {}", label_, describeHresult(result));
        return result;
    });
}

HRESULT ProxyEffect::Escape(LPDIEFFESCAPE escape) {
    return guarded("Escape", [&]() -> HRESULT {
        const HRESULT result = real_->Escape(escape);
        // Driver-specific commands: exactly what a Logitech-aware game might send to its wheel. Some games send
        // one every frame, so the log is sampled.
        static logging::CallSampler sampler(20, 1000);
        if (const auto call = sampler.next(); call != 0) {
            WE_LOG_INFO(log(), "{}: Escape #{} (command=0x{:x}, in={} bytes, out={} bytes) -> {}", label_, call,
                        escape != nullptr ? escape->dwCommand : 0, escape != nullptr ? escape->cbInBuffer : 0,
                        escape != nullptr ? escape->cbOutBuffer : 0, describeHresult(result));
        }
        return result;
    });
}

}  // namespace wheelbri::shim
