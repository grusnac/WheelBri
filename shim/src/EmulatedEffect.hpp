#pragma once

#include "DiFormat.hpp"
#include "ShimLogging.hpp"

#include "wheelemu/Ffb.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wheelemu::shim {

// Phase 3: a DirectInput effect of the emulated wheel. It keeps the parameters the game set (so
// GetParameters answers truthfully), turns them into an FfbEffect and hands that to FfbForwarder,
// which sends it to wheelemu-service. Nothing is played on the physical device from inside the game.
class EmulatedEffect final : public IDirectInputEffect {
public:
    // The FfbEffectType for a DirectInput effect GUID, or nullopt if the type is not supported.
    [[nodiscard]] static std::optional<FfbEffectType> typeFor(REFGUID guid);

    // `wheelAxisOffset` / `wheelAxisId`: how the game names the wheel axis in rgdwAxes, by data format
    // offset or by object id, so direction and per-axis conditions are read for the right axis.
    EmulatedEffect(REFGUID type, FfbEffectType effectType, std::uint8_t slot, std::string deviceLabel,
                   DWORD wheelAxisOffset, DWORD wheelAxisId);

    EmulatedEffect(const EmulatedEffect&) = delete;
    EmulatedEffect& operator=(const EmulatedEffect&) = delete;

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
    ~EmulatedEffect() = default;

    [[nodiscard]] FfbEffect toFfb() const;
    // A slot for this effect ("downloaded"), taking a free one if Unload gave the last one back.
    [[nodiscard]] bool ensureSlot();
    [[nodiscard]] std::size_t wheelAxisIndex() const;
    [[nodiscard]] float directionAlongWheel() const;

    GUID type_;
    FfbEffectType effectType_;
    // The service slot while the effect is downloaded. Unload gives it back, Download / SetParameters /
    // Start take one again: games such as NFS unload their effects between screens and never release the
    // objects, so tying slots to object lifetime ran out of slots after two screens.
    std::optional<std::uint8_t> slot_;
    std::string label_;
    DWORD wheelAxisOffset_;
    DWORD wheelAxisId_;
    std::atomic<ULONG> references_{1};

    // Parameters as the game set them (DIEFFECT, flattened).
    DWORD flags_ = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    DWORD duration_ = INFINITE;
    DWORD samplePeriod_ = 0;
    DWORD gain_ = DI_FFNOMINALMAX;
    DWORD triggerButton_ = DIEB_NOTRIGGER;
    DWORD triggerRepeatInterval_ = 0;
    DWORD startDelay_ = 0;
    std::vector<DWORD> axes_;
    std::vector<LONG> direction_;
    std::optional<DIENVELOPE> envelope_;
    std::vector<std::uint8_t> typeSpecific_;

    logging::CallSampler setSampler_{20, 500};
    bool warnedEnvelope_ = false;
};

}  // namespace wheelemu::shim
