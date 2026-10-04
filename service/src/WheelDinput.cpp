// The physical wheel on Windows, through DirectInput: listing the game controllers, reading the wheel
// (DinputReader) and its force feedback.
//
// The force feedback makes the same DirectInput calls the service always made (through SDL3 until it got its
// own code): the device
// held exclusively in the background, actuators reset and switched on, device gain 100 %, auto-centre off;
// every effect on the first force-feedback axis only (DIEFF_CARTESIAN, direction 0), started once and then
// updated in place with only the parameters that changed. The numbers come from WheelOutput.hpp.

#include "Wheel.hpp"

#include "DinputDevice.hpp"
#include "DinputReader.hpp"

#include "wheelbri/Logging.hpp"
#include "wheelbri/WheelOutput.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace wheelbri::service {

namespace {

const logging::Logger& inputLog() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

const logging::Logger& ffbLog() {
    static const logging::Logger logger = logging::get("service.ffb");
    return logger;
}

std::string hresult(const HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

const GUID* effectGuid(const FfbEffectType type) {
    switch (type) {
        case FfbEffectType::Constant: return &GUID_ConstantForce;
        case FfbEffectType::Spring: return &GUID_Spring;
        case FfbEffectType::Damper: return &GUID_Damper;
        case FfbEffectType::Friction: return &GUID_Friction;
        case FfbEffectType::Inertia: return &GUID_Inertia;
        case FfbEffectType::Sine: return &GUID_Sine;
        case FfbEffectType::Square: return &GUID_Square;
        case FfbEffectType::Triangle: return &GUID_Triangle;
        case FfbEffectType::SawtoothUp: return &GUID_SawtoothUp;
        case FfbEffectType::SawtoothDown: return &GUID_SawtoothDown;
    }
    return nullptr;
}

constexpr std::array<FfbEffectType, 10> kAllTypes = {
    FfbEffectType::Constant, FfbEffectType::Sine,         FfbEffectType::Square, FfbEffectType::Triangle,
    FfbEffectType::SawtoothUp, FfbEffectType::SawtoothDown, FfbEffectType::Spring, FfbEffectType::Damper,
    FfbEffectType::Inertia,  FfbEffectType::Friction,
};

// --- force feedback -----------------------------------------------------------------------------------

class DinputFfbSink final : public IFfbSink {
public:
    static std::unique_ptr<IFfbSink> open(const WheelInfo& wheel, const bool invert) {
        auto sink = std::unique_ptr<DinputFfbSink>(new DinputFfbSink(invert));
        std::string error;
        if (!sink->init(wheel, error)) {
            WE_LOG_ERROR(ffbLog(), "cannot open force feedback on '{}': {}", wheel.name, error);
            return nullptr;
        }
        return sink;
    }

    ~DinputFfbSink() override {
        if (device_ != nullptr) {
            if (ready_) stopAll();
            for (Slot& slot : slots_) {
                release(slot);
            }
            device_->Unacquire();
            device_->Release();
        }
        if (dinput_ != nullptr) dinput_->Release();
        if (window_ != nullptr) DestroyWindow(window_);
    }

    DinputFfbSink(const DinputFfbSink&) = delete;
    DinputFfbSink& operator=(const DinputFfbSink&) = delete;

    bool play(const std::size_t slotIndex, const FfbEffect& effect) override {
        if (slotIndex >= slots_.size()) {
            return false;
        }
        static logging::RateLimiter unsupportedLimiter(std::chrono::seconds(30));
        static logging::RateLimiter failureLimiter(std::chrono::seconds(5));
        if (supported_.count(effect.type) == 0) {
            WE_LOG_WARN_LIMITED(ffbLog(), unsupportedLimiter, "slot {}: the wheel cannot play {} effects, dropped",
                                slotIndex, ffbTypeName(effect.type));
            return false;
        }

        Slot& slot = slots_[slotIndex];
        const di::Output params = di::toDirectInput(toWheelEffect(effect, invert_));
        Parameters block;
        DIEFFECT description = describe(params, block);

        if (slot.effect != nullptr && slot.type == effect.type) {
            // Only what changed, as SDL does; everything if nothing did.
            const DWORD flags = params == slot.sent ? kAllParameters : DIEP_TYPESPECIFICPARAMS;
            const HRESULT result = retry([&] { return slot.effect->SetParameters(&description, flags); });
            if (FAILED(result)) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: updating {} effect failed ({})", slotIndex,
                                     ffbTypeName(effect.type), hresult(result));
                return false;
            }
        } else {
            release(slot);
            const HRESULT result = retry(
                [&] { return device_->CreateEffect(*effectGuid(effect.type), &description, &slot.effect, nullptr); });
            if (FAILED(result) || slot.effect == nullptr) {
                slot.effect = nullptr;
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: creating {} effect failed ({})", slotIndex,
                                     ffbTypeName(effect.type), hresult(result));
                return false;
            }
            slot.type = effect.type;
            WE_LOG_DEBUG(ffbLog(), "slot {}: created {} effect", slotIndex, ffbTypeName(effect.type));
        }
        slot.sent = params;

        if (!slot.running) {
            const HRESULT result = retry([&] { return slot.effect->Start(1, 0); });
            if (FAILED(result)) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: starting {} effect failed ({})", slotIndex,
                                     ffbTypeName(effect.type), hresult(result));
                return false;
            }
            slot.running = true;
            WE_LOG_DEBUG(ffbLog(), "slot {}: {} effect started", slotIndex, ffbTypeName(effect.type));
        }
        return true;
    }

    void stop(const std::size_t slotIndex) override {
        if (slotIndex >= slots_.size()) {
            return;
        }
        Slot& slot = slots_[slotIndex];
        if (slot.effect != nullptr && slot.running) {
            const HRESULT result = slot.effect->Stop();
            if (FAILED(result)) {
                WE_LOG_WARN(ffbLog(), "slot {}: stopping effect failed ({})", slotIndex, hresult(result));
            }
            slot.running = false;
            WE_LOG_DEBUG(ffbLog(), "slot {}: stopped", slotIndex);
        }
    }

    void stopAll() override {
        // Marked stopped first: if the command fails because the wheel was lost, retry() takes it back and restarts
        // what is still marked running, which here would push the last forces again for a moment before stopping.
        for (Slot& slot : slots_) {
            slot.running = false;
        }
        const HRESULT result = retry([&] { return device_->SendForceFeedbackCommand(DISFFC_STOPALL); });
        if (FAILED(result)) {
            WE_LOG_WARN(ffbLog(), "stopping all effects failed ({}); stopping them one by one", hresult(result));
            for (Slot& slot : slots_) {
                if (slot.effect != nullptr) slot.effect->Stop();
            }
        } else {
            WE_LOG_DEBUG(ffbLog(), "all effects stopped");
        }
    }

private:
    static constexpr DWORD kAllParameters = DIEP_DIRECTION | DIEP_DURATION | DIEP_ENVELOPE | DIEP_STARTDELAY |
                                            DIEP_TRIGGERBUTTON | DIEP_TRIGGERREPEATINTERVAL |
                                            DIEP_TYPESPECIFICPARAMS;

    struct Slot {
        IDirectInputEffect* effect = nullptr;
        FfbEffectType type = FfbEffectType::Constant;
        di::Output sent;
        bool running = false;
    };

    // The type-specific block a DIEFFECT points to.
    struct Parameters {
        DICONSTANTFORCE constant{};
        DICONDITION condition{};
        DIPERIODIC periodic{};
        DWORD axis = 0;
        LONG direction = 0;
    };

    explicit DinputFfbSink(const bool invert) : invert_(invert) {}

    bool init(const WheelInfo& wheel, std::string& error) {
        if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                      reinterpret_cast<void**>(&dinput_), nullptr))) {
            error = "DirectInput8Create failed";
            return false;
        }
        device_ = dinput::createByPath(dinput_, wheel.path);
        if (device_ == nullptr) {
            error = "it is no longer connected";
            return false;
        }
        window_ = dinput::createHelperWindow(L"wheelbri-ffb");
        HRESULT result = window_ == nullptr ? E_FAIL
                                            : device_->SetCooperativeLevel(window_, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
        if (FAILED(result)) {
            error = "SetCooperativeLevel(exclusive, background) failed (" + hresult(result) +
                    "); is another program using the wheel's force feedback?";
            return false;
        }
        if (FAILED(result = device_->SetDataFormat(&c_dfDIJoystick2))) {
            error = "SetDataFormat failed (" + hresult(result) + ")";
            return false;
        }
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        if (SUCCEEDED(device_->GetCapabilities(&caps)) && (caps.dwFlags & DIDC_FORCEFEEDBACK) == 0) {
            error = "DirectInput reports no force feedback for it";
            return false;
        }
        if (FAILED(result = device_->Acquire())) {
            error = "Acquire failed (" + hresult(result) + "); is another program using the wheel's force feedback?";
            return false;
        }
        device_->EnumObjects(&DinputFfbSink::collectAxis, this, DIDFT_AXIS);
        if (!axisFound_) {
            error = "it has no force-feedback axis";
            return false;
        }
        if (FAILED(result = device_->SendForceFeedbackCommand(DISFFC_RESET))) {
            error = "resetting its actuators failed (" + hresult(result) + ")";
            return false;
        }
        if (FAILED(result = device_->SendForceFeedbackCommand(DISFFC_SETACTUATORSON))) {
            error = "switching its actuators on failed (" + hresult(result) + ")";
            return false;
        }
        device_->EnumEffects(&DinputFfbSink::collectEffect, this, DIEFT_ALL);
        if (supported_.empty()) {
            error = "it supports none of the effects";
            return false;
        }
        // The user's gain lives in the limiter; the game's spring does the centring.
        const bool gain = setDword(DIPROP_FFGAIN, 10000);
        const bool autocenter = setDword(DIPROP_AUTOCENTER, DIPROPAUTOCENTER_OFF);

        std::string names;
        for (const FfbEffectType type : kAllTypes) {
            if (supported_.count(type) != 0) {
                names += (names.empty() ? "" : ", ") + std::string(ffbTypeName(type));
            }
        }
        ready_ = true;
        WE_LOG_INFO(ffbLog(), "force feedback on '{}' (DirectInput): {}{}{}, invert={}", wheel.name, names,
                    gain ? ", gain" : "", autocenter ? ", autocenter off" : "", invert_);
        if (supported_.count(FfbEffectType::Constant) == 0 && supported_.count(FfbEffectType::Spring) == 0 &&
            supported_.count(FfbEffectType::Damper) == 0) {
            WE_LOG_WARN(ffbLog(), "device reports none of constant/spring/damper; racing-game force feedback will be "
                                  "missing");
        }
        return true;
    }

    static BOOL CALLBACK collectAxis(LPCDIDEVICEOBJECTINSTANCEW object, LPVOID context) {
        auto* self = static_cast<DinputFfbSink*>(context);
        const GUID& type = object->guidType;
        const bool knownAxis = IsEqualGUID(type, GUID_XAxis) || IsEqualGUID(type, GUID_YAxis) ||
                               IsEqualGUID(type, GUID_ZAxis) || IsEqualGUID(type, GUID_RxAxis) ||
                               IsEqualGUID(type, GUID_RyAxis) || IsEqualGUID(type, GUID_RzAxis);
        if ((object->dwType & DIDFT_AXIS) != 0 && (object->dwFlags & DIDOI_FFACTUATOR) != 0 && knownAxis) {
            self->axis_ = object->dwOfs;   // the first one is the steering axis
            self->axisFound_ = true;
            return DIENUM_STOP;
        }
        return DIENUM_CONTINUE;
    }

    static BOOL CALLBACK collectEffect(LPCDIEFFECTINFOW info, LPVOID context) {
        auto* self = static_cast<DinputFfbSink*>(context);
        for (const FfbEffectType type : kAllTypes) {
            if (IsEqualGUID(info->guid, *effectGuid(type))) {
                self->supported_.insert(type);
            }
        }
        return DIENUM_CONTINUE;
    }

    bool setDword(REFGUID property, const DWORD value) {
        DIPROPDWORD dword{};
        dword.diph.dwSize = sizeof(dword);
        dword.diph.dwHeaderSize = sizeof(dword.diph);
        dword.diph.dwObj = 0;
        dword.diph.dwHow = DIPH_DEVICE;
        dword.dwData = value;
        return SUCCEEDED(device_->SetProperty(property, &dword.diph));
    }

    DIEFFECT describe(const di::Output& p, Parameters& block) const {
        block.axis = axis_;
        block.direction = 0;
        DIEFFECT e{};
        e.dwSize = sizeof(e);
        e.dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
        e.dwDuration = INFINITE;
        e.dwSamplePeriod = 0;
        e.dwGain = DI_FFNOMINALMAX;
        e.dwTriggerButton = DIEB_NOTRIGGER;
        e.dwTriggerRepeatInterval = 0;
        e.cAxes = 1;
        e.rgdwAxes = &block.axis;
        e.rglDirection = &block.direction;
        e.lpEnvelope = nullptr;
        e.dwStartDelay = 0;
        switch (p.block) {
            case di::Block::Constant:
                block.constant.lMagnitude = p.magnitude;
                e.cbTypeSpecificParams = sizeof(block.constant);
                e.lpvTypeSpecificParams = &block.constant;
                break;
            case di::Block::Condition:
                block.condition.lOffset = p.offset;
                block.condition.lPositiveCoefficient = p.positiveCoefficient;
                block.condition.lNegativeCoefficient = p.negativeCoefficient;
                block.condition.dwPositiveSaturation = p.positiveSaturation;
                block.condition.dwNegativeSaturation = p.negativeSaturation;
                block.condition.lDeadBand = p.deadBand;
                e.cbTypeSpecificParams = sizeof(block.condition);
                e.lpvTypeSpecificParams = &block.condition;
                break;
            case di::Block::Periodic:
                block.periodic.dwMagnitude = p.periodicMagnitude;
                block.periodic.lOffset = p.periodicOffset;
                block.periodic.dwPhase = p.phase;
                block.periodic.dwPeriod = p.periodUs;
                e.cbTypeSpecificParams = sizeof(block.periodic);
                e.lpvTypeSpecificParams = &block.periodic;
                break;
        }
        return e;
    }

    // A call that failed because the device lost its exclusive access (another program took it, or Windows
    // dropped it): take it back and try once more, as SDL does. Losing the device unloads every effect, so the
    // ones that were playing are started again (SDL left them silent until the game restarted them).
    template <typename Call>
    HRESULT retry(Call call) {
        HRESULT result = call();
        if (result == static_cast<HRESULT>(DIERR_NOTEXCLUSIVEACQUIRED)) {
            device_->Unacquire();
            const HRESULT level = device_->SetCooperativeLevel(window_, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
            if (FAILED(level)) {
                return level;
            }
            result = DIERR_NOTACQUIRED;
        }
        // DI_DOWNLOADSKIPPED: the parameters were stored but not sent, because the device is not ours.
        if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED || result == DI_DOWNLOADSKIPPED) {
            if (SUCCEEDED(device_->Acquire())) {
                WE_LOG_INFO(ffbLog(), "force feedback: the wheel was taken back after losing it; restarting effects");
                restartPlaying();
                result = call();
            }
        }
        return result;
    }

    void restartPlaying() {
        for (Slot& slot : slots_) {
            if (slot.effect != nullptr && slot.running && FAILED(slot.effect->Start(1, 0))) {
                slot.running = false;   // started again by the next play()
            }
        }
    }

    void release(Slot& slot) {
        if (slot.effect != nullptr) {
            slot.effect->Unload();
            slot.effect->Release();
        }
        slot = Slot{};
    }

    bool invert_;
    IDirectInput8W* dinput_ = nullptr;
    IDirectInputDevice8W* device_ = nullptr;
    HWND window_ = nullptr;
    DWORD axis_ = 0;
    bool axisFound_ = false;
    bool ready_ = false;
    std::set<FfbEffectType> supported_;
    std::array<Slot, kMaxSlots> slots_{};
};

// --- the wheel ----------------------------------------------------------------------------------------

class DinputWheel final : public Wheel {
public:
    DinputWheel(WheelInfo info, std::unique_ptr<DinputReader> reader)
        : info_(std::move(info)), reader_(std::move(reader)) {}

    [[nodiscard]] const WheelInfo& info() const override { return info_; }
    [[nodiscard]] int axes() const override { return reader_->axes(); }
    [[nodiscard]] int buttons() const override { return reader_->buttons(); }
    [[nodiscard]] int hats() const override { return reader_->hats(); }

    bool poll(RawInputState& state) override {
        if (!reader_->read(state)) {
            WE_LOG_ERROR(inputLog(), "'{}' was disconnected (DirectInput cannot read it any more)", info_.name);
            return false;
        }
        return true;
    }

    std::unique_ptr<IFfbSink> openForceFeedback(const bool invert) override {
        return DinputFfbSink::open(info_, invert);
    }

private:
    WheelInfo info_;
    std::unique_ptr<DinputReader> reader_;
};

class DinputBackend final : public WheelBackend {
public:
    explicit DinputBackend(IDirectInput8W* dinput) : dinput_(dinput) {}
    ~DinputBackend() override { dinput_->Release(); }

    DinputBackend(const DinputBackend&) = delete;
    DinputBackend& operator=(const DinputBackend&) = delete;

    std::vector<WheelInfo> list() override {
        std::vector<WheelInfo> wheels;
        struct Context {
            IDirectInput8W* dinput;
            std::vector<WheelInfo>* wheels;
        } context{dinput_, &wheels};
        dinput_->EnumDevices(
            DI8DEVCLASS_GAMECTRL,
            [](LPCDIDEVICEINSTANCEW instance, LPVOID data) -> BOOL {
                auto* c = static_cast<Context*>(data);
                WheelInfo info;
                info.name = dinput::toUtf8(instance->tszProductName);
                if (info.name.empty()) info.name = "(unnamed)";
                // HID devices: guidProduct is { MAKELONG(vid, pid), ..., "PIDVID" }.
                if (std::memcmp(&instance->guidProduct.Data4[2], "PIDVID", 6) == 0) {
                    info.vendorId = LOWORD(instance->guidProduct.Data1);
                    info.productId = HIWORD(instance->guidProduct.Data1);
                }
                IDirectInputDevice8W* device = nullptr;
                if (SUCCEEDED(c->dinput->CreateDevice(instance->guidInstance, &device, nullptr))) {
                    info.path = dinput::toUtf8(dinput::hidPath(device).c_str());
                    DIPROPDWORD vidpid{};
                    vidpid.diph.dwSize = sizeof(vidpid);
                    vidpid.diph.dwHeaderSize = sizeof(vidpid.diph);
                    vidpid.diph.dwHow = DIPH_DEVICE;
                    if (info.vendorId == 0 && SUCCEEDED(device->GetProperty(DIPROP_VIDPID, &vidpid.diph))) {
                        info.vendorId = LOWORD(vidpid.dwData);
                        info.productId = HIWORD(vidpid.dwData);
                    }
                    device->Release();
                }
                c->wheels->push_back(std::move(info));
                return DIENUM_CONTINUE;
            },
            &context, DIEDFL_ATTACHEDONLY);
        WE_LOG_DEBUG(inputLog(), "{} game controller(s) connected", wheels.size());
        return wheels;
    }

    std::unique_ptr<Wheel> open(const WheelInfo& info) override {
        if (info.path.empty()) {
            WE_LOG_ERROR(inputLog(), "cannot open '{}': DirectInput gives no device path for it", info.name);
            return nullptr;
        }
        std::string error;
        auto reader = DinputReader::open(info.path, error);
        if (reader == nullptr) {
            WE_LOG_ERROR(inputLog(), "cannot open '{}': {}", info.name, error);
            return nullptr;
        }
        return std::make_unique<DinputWheel>(info, std::move(reader));
    }

    [[nodiscard]] std::string describe() const override { return "DirectInput"; }

private:
    IDirectInput8W* dinput_;
};

}  // namespace

std::unique_ptr<WheelBackend> startWheelBackend() {
    IDirectInput8W* dinput = nullptr;
    const HRESULT result = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                              reinterpret_cast<void**>(&dinput), nullptr);
    if (FAILED(result)) {
        WE_LOG_CRITICAL(inputLog(), "DirectInput8Create failed ({})", hresult(result));
        return nullptr;
    }
    return std::make_unique<DinputBackend>(dinput);
}

}  // namespace wheelbri::service
