#pragma once

#include "DiFormat.hpp"
#include "ShimLogging.hpp"

#include <atomic>
#include <string>

namespace wheelbri::shim {

// Logs every force feedback call the game makes on one effect, then forwards it unchanged.
class ProxyEffect final : public IDirectInputEffect {
public:
    ProxyEffect(IDirectInputEffect* real, const GUID& type, std::string deviceLabel);

    ProxyEffect(const ProxyEffect&) = delete;
    ProxyEffect& operator=(const ProxyEffect&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE instance, DWORD version, REFGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID guid) override;
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT effect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT effect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD status) override;
    HRESULT STDMETHODCALLTYPE Download() override;
    HRESULT STDMETHODCALLTYPE Unload() override;
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override;

private:
    ~ProxyEffect() = default;

    IDirectInputEffect* real_;
    GUID type_;
    std::string label_;
    std::atomic<ULONG> references_{1};
    logging::CallSampler setParametersSampler_{30, 500};
    logging::CallSampler statusSampler_{5, 1000};
};

// Replaces the real effect in *out with a ProxyEffect around it. *out is cleared first, so if the wrapper cannot be
// made the game never gets the unwrapped object: the real effect is released and the exception goes on to guarded().
void wrapRealEffect(LPDIRECTINPUTEFFECT* out, const GUID& type, const std::string& deviceLabel);

}  // namespace wheelbri::shim
