#include "EmulatedEffect.hpp"

#include "FfbForwarder.hpp"

#include "wheelemu/DiEffect.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace wheelemu::shim {

static_assert(di::kCartesian == DIEFF_CARTESIAN);
static_assert(di::kPolar == DIEFF_POLAR);
static_assert(di::kSpherical == DIEFF_SPHERICAL);

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.effect");
    return instance;
}

template <typename T>
bool readTypeSpecific(const std::vector<std::uint8_t>& bytes, const std::size_t index, T& out) {
    if (bytes.size() < (index + 1) * sizeof(T)) {
        return false;
    }
    std::memcpy(&out, bytes.data() + index * sizeof(T), sizeof(T));
    return true;
}

}  // namespace

std::optional<FfbEffectType> EmulatedEffect::typeFor(REFGUID guid) {
    if (guid == GUID_ConstantForce || guid == GUID_RampForce) return FfbEffectType::Constant;
    if (guid == GUID_Spring) return FfbEffectType::Spring;
    if (guid == GUID_Damper) return FfbEffectType::Damper;
    if (guid == GUID_Friction) return FfbEffectType::Friction;
    if (guid == GUID_Inertia) return FfbEffectType::Inertia;
    if (guid == GUID_Sine) return FfbEffectType::Sine;
    if (guid == GUID_Square) return FfbEffectType::Square;
    if (guid == GUID_Triangle) return FfbEffectType::Triangle;
    if (guid == GUID_SawtoothUp) return FfbEffectType::SawtoothUp;
    if (guid == GUID_SawtoothDown) return FfbEffectType::SawtoothDown;
    return std::nullopt;
}

EmulatedEffect::EmulatedEffect(REFGUID type, const FfbEffectType effectType, const std::uint8_t slot,
                               std::string deviceLabel, const DWORD wheelAxisOffset, const DWORD wheelAxisId)
    : type_(type),
      effectType_(effectType),
      slot_(slot),
      label_(std::move(deviceLabel) + " effect " + describeGuid(type)),
      wheelAxisOffset_(wheelAxisOffset),
      wheelAxisId_(wheelAxisId) {
    WE_LOG_DEBUG(log(), "{}: created in slot {}", label_, slot);
}

bool EmulatedEffect::ensureSlot() {
    if (slot_) {
        return true;
    }
    slot_ = FfbForwarder::instance().allocate();
    if (!slot_) {
        return false;
    }
    FfbForwarder::instance().setEffect(*slot_, toFfb());
    WE_LOG_DEBUG(log(), "{}: downloaded again, slot {}", label_, *slot_);
    return true;
}

HRESULT EmulatedEffect::QueryInterface(REFIID riid, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDirectInputEffect) {
        *object = this;
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG EmulatedEffect::AddRef() { return ++references_; }

ULONG EmulatedEffect::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) {
        if (slot_) {
            FfbForwarder::instance().release(*slot_);
        }
        WE_LOG_DEBUG(log(), "{}: released", label_);
        delete this;
    }
    return remaining;
}

HRESULT EmulatedEffect::Initialize(HINSTANCE, DWORD, REFGUID) { return DI_OK; }

HRESULT EmulatedEffect::GetEffectGuid(LPGUID guid) {
    if (guid == nullptr) {
        return E_POINTER;
    }
    *guid = type_;
    return DI_OK;
}

std::size_t EmulatedEffect::wheelAxisIndex() const {
    const DWORD wanted = (flags_ & DIEFF_OBJECTIDS) != 0 ? wheelAxisId_ : wheelAxisOffset_;
    for (std::size_t i = 0; i < axes_.size(); ++i) {
        // Object ids may carry extra flag bits (DIDFT_FFACTUATOR); compare kind and instance only.
        const DWORD mask = (flags_ & DIEFF_OBJECTIDS) != 0 ? 0x00FFFFFF : 0xFFFFFFFF;
        if ((axes_[i] & mask) == (wanted & mask)) {
            return i;
        }
    }
    return 0;   // single-axis effects on a wheel are for the wheel
}

float EmulatedEffect::directionAlongWheel() const {
    if (direction_.empty()) {
        return 1.0F;
    }
    if ((flags_ & DIEFF_CARTESIAN) != 0 && direction_.size() > 1) {
        // Share of the direction vector that lies on the wheel axis.
        double norm = 0.0;
        for (const LONG component : direction_) {
            norm += static_cast<double>(component) * static_cast<double>(component);
        }
        const std::size_t index = std::min(wheelAxisIndex(), direction_.size() - 1);
        return norm > 0.0 ? static_cast<float>(direction_[index] / std::sqrt(norm)) : 1.0F;
    }
    return di::directionAlongX(flags_, direction_.front());
}

FfbEffect EmulatedEffect::toFfb() const {
    const float direction = directionAlongWheel();
    switch (effectType_) {
        case FfbEffectType::Constant: {
            if (type_ == GUID_RampForce) {
                DIRAMPFORCE ramp{};
                readTypeSpecific(typeSpecific_, 0, ramp);
                // A ramp is played as its average level; games use it rarely and briefly.
                return di::constantForce((ramp.lStart + ramp.lEnd) / 2, direction, gain_);
            }
            DICONSTANTFORCE constant{};
            readTypeSpecific(typeSpecific_, 0, constant);
            return di::constantForce(constant.lMagnitude, direction, gain_);
        }
        case FfbEffectType::Spring:
        case FfbEffectType::Damper:
        case FfbEffectType::Friction:
        case FfbEffectType::Inertia: {
            // One DICONDITION per axis, or one for all axes (in the effect's direction).
            DICONDITION condition{};
            if (!readTypeSpecific(typeSpecific_, wheelAxisIndex(), condition)) {
                readTypeSpecific(typeSpecific_, 0, condition);
            }
            return di::condition(effectType_, condition.lOffset, condition.lPositiveCoefficient,
                                 condition.lNegativeCoefficient, condition.dwPositiveSaturation,
                                 condition.dwNegativeSaturation, condition.lDeadBand, gain_);
        }
        default: {
            DIPERIODIC periodic{};
            readTypeSpecific(typeSpecific_, 0, periodic);
            return di::periodic(effectType_, periodic.dwMagnitude, periodic.lOffset, periodic.dwPhase,
                                periodic.dwPeriod, direction, gain_);
        }
    }
}

HRESULT EmulatedEffect::SetParameters(LPCDIEFFECT effect, const DWORD flags) {
    if (effect == nullptr || (effect->dwSize != sizeof(DIEFFECT) && effect->dwSize != sizeof(DIEFFECT_DX5))) {
        return DIERR_INVALIDPARAM;
    }
    if ((flags & DIEP_DURATION) != 0) duration_ = effect->dwDuration;
    if ((flags & DIEP_SAMPLEPERIOD) != 0) samplePeriod_ = effect->dwSamplePeriod;
    if ((flags & DIEP_GAIN) != 0) gain_ = std::min<DWORD>(effect->dwGain, DI_FFNOMINALMAX);
    if ((flags & DIEP_TRIGGERBUTTON) != 0) triggerButton_ = effect->dwTriggerButton;
    if ((flags & DIEP_TRIGGERREPEATINTERVAL) != 0) triggerRepeatInterval_ = effect->dwTriggerRepeatInterval;
    if ((flags & DIEP_STARTDELAY) != 0 && effect->dwSize == sizeof(DIEFFECT)) startDelay_ = effect->dwStartDelay;
    if ((flags & (DIEP_AXES | DIEP_DIRECTION)) != 0) {
        flags_ = effect->dwFlags;
    }
    if ((flags & DIEP_AXES) != 0) {
        if (effect->cAxes != 0 && effect->rgdwAxes == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        axes_.assign(effect->rgdwAxes, effect->rgdwAxes + effect->cAxes);
    }
    if ((flags & DIEP_DIRECTION) != 0) {
        const DWORD count = effect->cAxes != 0 ? effect->cAxes : static_cast<DWORD>(axes_.size());
        if (count != 0 && effect->rglDirection == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        direction_.assign(effect->rglDirection, effect->rglDirection + count);
    }
    if ((flags & DIEP_ENVELOPE) != 0) {
        if (effect->lpEnvelope != nullptr) {
            envelope_ = *effect->lpEnvelope;
            if (!warnedEnvelope_) {
                warnedEnvelope_ = true;
                WE_LOG_DEBUG(log(), "{}: envelope (attack/fade) is ignored; the effect plays at full level", label_);
            }
        } else {
            envelope_.reset();
        }
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) != 0) {
        if (effect->cbTypeSpecificParams != 0 && effect->lpvTypeSpecificParams == nullptr) {
            return DIERR_INVALIDPARAM;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(effect->lpvTypeSpecificParams);
        typeSpecific_.assign(bytes, bytes + effect->cbTypeSpecificParams);
    }

    const FfbEffect converted = toFfb();
    // SetParameters downloads the effect unless told not to (DIEP_NODOWNLOAD).
    if ((flags & DIEP_NODOWNLOAD) == 0 && !ensureSlot()) {
        return DIERR_DEVICEFULL;
    }
    if (slot_) {
        FfbForwarder::instance().setEffect(*slot_, converted);
    }
    if (const auto call = setSampler_.next(); call != 0) {
        WE_LOG_DEBUG(log(), "{}: SetParameters #{} flags=0x{:x} -> level={:+.3f} coef={:+.3f}/{:+.3f} "
                            "sat={:.3f}/{:.3f} periodic={:.3f}@{:.0f}ms gain={:.2f}",
                     label_, call, flags, converted.constantLevel, converted.condition.positiveCoefficient,
                     converted.condition.negativeCoefficient, converted.condition.positiveSaturation,
                     converted.condition.negativeSaturation, converted.periodic.magnitude,
                     converted.periodic.periodMs, converted.gain);
    }
    if ((flags & DIEP_START) != 0) {
        return Start(1, 0);
    }
    return DI_OK;
}

HRESULT EmulatedEffect::GetParameters(LPDIEFFECT effect, const DWORD flags) {
    if (effect == nullptr || (effect->dwSize != sizeof(DIEFFECT) && effect->dwSize != sizeof(DIEFFECT_DX5))) {
        return DIERR_INVALIDPARAM;
    }
    HRESULT result = DI_OK;
    if ((flags & DIEP_DURATION) != 0) effect->dwDuration = duration_;
    if ((flags & DIEP_SAMPLEPERIOD) != 0) effect->dwSamplePeriod = samplePeriod_;
    if ((flags & DIEP_GAIN) != 0) effect->dwGain = gain_;
    if ((flags & DIEP_TRIGGERBUTTON) != 0) effect->dwTriggerButton = triggerButton_;
    if ((flags & DIEP_TRIGGERREPEATINTERVAL) != 0) effect->dwTriggerRepeatInterval = triggerRepeatInterval_;
    if ((flags & DIEP_STARTDELAY) != 0 && effect->dwSize == sizeof(DIEFFECT)) effect->dwStartDelay = startDelay_;
    if ((flags & (DIEP_AXES | DIEP_DIRECTION)) != 0) {
        const DWORD needed = static_cast<DWORD>(std::max(axes_.size(), direction_.size()));
        if (effect->cAxes < needed) {
            effect->cAxes = needed;
            return DIERR_MOREDATA;
        }
        effect->cAxes = needed;
        effect->dwFlags = (effect->dwFlags & ~(DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL)) |
                          (flags_ & (DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL));
        if ((flags & DIEP_AXES) != 0 && effect->rgdwAxes != nullptr) {
            std::copy(axes_.begin(), axes_.end(), effect->rgdwAxes);
        }
        if ((flags & DIEP_DIRECTION) != 0 && effect->rglDirection != nullptr) {
            std::copy(direction_.begin(), direction_.end(), effect->rglDirection);
        }
    }
    if ((flags & DIEP_ENVELOPE) != 0) {
        if (envelope_ && effect->lpEnvelope != nullptr) {
            *effect->lpEnvelope = *envelope_;
        } else {
            effect->lpEnvelope = nullptr;
        }
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) != 0) {
        if (effect->cbTypeSpecificParams < typeSpecific_.size()) {
            effect->cbTypeSpecificParams = static_cast<DWORD>(typeSpecific_.size());
            result = DIERR_MOREDATA;
        } else if (effect->lpvTypeSpecificParams != nullptr) {
            std::memcpy(effect->lpvTypeSpecificParams, typeSpecific_.data(), typeSpecific_.size());
            effect->cbTypeSpecificParams = static_cast<DWORD>(typeSpecific_.size());
        }
    }
    return result;
}

HRESULT EmulatedEffect::Start(const DWORD iterations, const DWORD flags) {
    const std::optional<std::uint64_t> duration =
        duration_ == INFINITE ? std::nullopt : std::optional<std::uint64_t>(duration_);
    const std::optional<std::uint32_t> count =
        iterations == INFINITE ? std::nullopt : std::optional<std::uint32_t>(std::max<DWORD>(iterations, 1));
    if (!ensureSlot()) {
        return DIERR_DEVICEFULL;
    }
    FfbForwarder::instance().start(*slot_, duration, count, startDelay_, (flags & DIES_SOLO) != 0);
    WE_LOG_DEBUG(log(), "{}: Start(iterations={}, flags=0x{:x})", label_,
                 iterations == INFINITE ? std::string("INFINITE") : std::to_string(iterations), flags);
    return DI_OK;
}

HRESULT EmulatedEffect::Stop() {
    if (slot_) {
        FfbForwarder::instance().stop(*slot_);
    }
    WE_LOG_DEBUG(log(), "{}: Stop", label_);
    return DI_OK;
}

HRESULT EmulatedEffect::GetEffectStatus(LPDWORD status) {
    if (status == nullptr) {
        return E_POINTER;
    }
    *status = slot_ && FfbForwarder::instance().isPlaying(*slot_) ? DIEGES_PLAYING : 0;
    return DI_OK;
}

HRESULT EmulatedEffect::Download() {
    return ensureSlot() ? DI_OK : DIERR_DEVICEFULL;
}

HRESULT EmulatedEffect::Unload() {
    // Stops the effect and frees its slot (the object stays valid and can be downloaded again).
    if (slot_) {
        const std::uint8_t slot = *slot_;
        FfbForwarder::instance().release(slot);
        slot_.reset();
        WE_LOG_DEBUG(log(), "{}: Unload, slot {} freed", label_, slot);
    }
    return DI_OK;
}

HRESULT EmulatedEffect::Escape(LPDIEFFESCAPE) { return DIERR_UNSUPPORTED; }

}  // namespace wheelemu::shim
