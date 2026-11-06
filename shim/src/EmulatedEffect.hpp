// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DiFormat.hpp"
#include "FfbForwarder.hpp"
#include "ShimLogging.hpp"

#include "wheelbri/Ffb.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::shim {

// A DirectInput effect of the emulated wheel. It keeps the parameters the game set (so
// GetParameters answers truthfully), turns them into an FfbEffect and hands that to FfbForwarder,
// which sends it to wheelbri-service. Nothing is played on the physical device from inside the game.
class EmulatedEffect final : public IDirectInputEffect {
public:
    // The FfbEffectType for a DirectInput effect GUID, or nullopt if the type is not supported.
    [[nodiscard]] static std::optional<FfbEffectType> typeFor(REFGUID guid);

    // `wheelAxisOffset` / `wheelAxisId`: how the game names the wheel axis in rgdwAxes, by data format
    // offset or by object id, so direction and per-axis conditions are read for the right axis.
    // `owner`: the device that created it (FfbForwarder keeps each device's effects apart).
    EmulatedEffect(REFGUID type, FfbEffectType effectType, FfbForwarder::Allocation slot, const void* owner,
                   std::string deviceLabel, DWORD wheelAxisOffset, DWORD wheelAxisId);

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
    // Logs a constant force's magnitude and direction as the game passed them, when they changed. Caller holds mutex_.
    void logRawConstant(const FfbEffect& converted);
    // Its envelope (attack / fade) and a ramp's end level, which FfbForwarder applies as it plays.
    [[nodiscard]] FfbShape toShape() const;
    // A slot for this effect ("downloaded"), taking a free one if Unload or a device reset gave the last one back.
    [[nodiscard]] bool ensureSlot();
    // The slot it still holds, or nullopt (never downloaded, unloaded, or freed by the device's reset).
    [[nodiscard]] std::optional<FfbForwarder::Allocation> currentSlot();
    [[nodiscard]] std::size_t wheelAxisIndex() const;
    [[nodiscard]] float directionAlongWheel() const;

    GUID type_;
    FfbEffectType effectType_;
    // The service slot while the effect is downloaded. Unload gives it back, Download / SetParameters /
    // Start take one again: games such as NFS unload their effects between screens and never release the
    // objects, so tying slots to object lifetime ran out of slots after two screens.
    std::optional<FfbForwarder::Allocation> slot_;
    const void* owner_;
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
    // What the game passed for a constant force, logged each time it changes (the first kMaxRawChanges times): its
    // magnitude and first direction as given, before they become a level. A game may say how strong a force is in
    // more than the magnitude (Race Driver: GRID's level was at full every frame).
    static constexpr int kMaxRawChanges = 300;
    long lastRawMagnitude_ = 0;
    long lastRawDirection_ = 0;
    bool haveRaw_ = false;
    int rawChanges_ = 0;
    bool warnedGarbage_ = false;   // a magnitude far outside -10000..10000 was reported (once)
    logging::CallSampler startStopSampler_{20, 500};
    // Games call an effect from several threads (input and force feedback). Recursive: SetParameters(DIEP_START)
    // calls Start.
    std::recursive_mutex mutex_;
};

}  // namespace wheelbri::shim
