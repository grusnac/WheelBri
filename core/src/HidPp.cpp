#include "wheelbri/HidPp.hpp"

#include "wheelbri/HidPpFfb.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace wheelbri {

namespace {

std::uint16_t be16(const std::span<const std::uint8_t> p, const std::size_t at) {
    if (at + 1 >= p.size()) return 0;
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[at]) << 8U) | p[at + 1]);
}

void putBe16(const std::span<std::uint8_t> p, const std::size_t at, const std::uint16_t value) {
    if (at + 1 >= p.size()) return;
    p[at] = static_cast<std::uint8_t>(value >> 8U);
    p[at + 1] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::string featureName(const std::uint16_t id) {
    switch (id) {
        case HidPp::kRoot: return "Root";
        case HidPp::kFeatureSet: return "FeatureSet";
        case HidPp::kFwVersion: return "DeviceFwVersion";
        case HidPp::kNameType: return "DeviceNameType";
        case HidPp::kForceFeedback: return "ForceFeedback";
        default: break;
    }
    char text[8];
    std::snprintf(text, sizeof(text), "0x%04X", static_cast<unsigned>(id));
    return text;
}

}  // namespace

HidPp::HidPp(Identity identity, Logger log, Settings settings)
    : identity_(std::move(identity)), log_(std::move(log)), settings_(std::move(settings)),
      features_{{kRoot, 0}, {kFeatureSet, 1}, {kFwVersion, 2}, {kNameType, 1}, {kForceFeedback, 0}} {}

void HidPp::setWheelRotation(const int degrees) {
    if (degrees < 40 || degrees > 2520) return;
    std::lock_guard lock(mutex_);
    if (degrees == wheelRotationDeg_) return;
    wheelRotationDeg_ = degrees;
    if (log_) {
        log_(LogLevel::Info, "hid++: the wheel turns " + std::to_string(degrees) + " deg (reported as the rotation)");
    }
}

int HidPp::rotationDeg() const {
    std::lock_guard lock(mutex_);
    return rotationDeg_;
}

float HidPp::gain() const {
    std::lock_guard lock(mutex_);
    return static_cast<float>(gain_) / 65535.0F;
}

std::uint64_t HidPp::requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
}

std::string HidPp::hex(const std::span<const std::uint8_t> bytes) {
    std::string text;
    char b[4];
    for (const std::uint8_t byte : bytes) {
        std::snprintf(b, sizeof(b), "%02x ", static_cast<unsigned>(byte));
        text += b;
    }
    if (!text.empty()) text.pop_back();
    return text;
}

void HidPp::showCall(const std::uint8_t function, const std::string& what, const Params in) {
    if (!log_) return;
    int& shown = shown_[function & 0x0FU];
    const bool full = shown < kShownCalls;
    if (full) ++shown;
    const bool last = full && shown == kShownCalls;
    log_(full ? LogLevel::Info : LogLevel::Debug,
         "hid++: force feedback " + what + ": " + hex(in) + (last ? " (the next ones at debug)" : ""));
}

void HidPp::note(const std::uint8_t featureIndex, const std::uint8_t function, const std::string& what) {
    if (!log_) return;
    if (seen_.emplace(featureIndex, function).second) log_(LogLevel::Info, "hid++: " + what);
}

void HidPp::setForceFeedback(HidPpFfb* ffb) {
    std::lock_guard lock(mutex_);
    ffb_ = ffb;
}

std::optional<std::vector<std::uint8_t>> HidPp::handle(const std::span<const std::uint8_t> report,
                                                       const std::int64_t nowMs) {
    if (report.size() < 4 || !isHidPpReport(report[0])) return std::nullopt;
    // Every answer is very long, whatever the request's size: a real G923 answers its long requests in 0x12 too
    // (292 of 292 in the 2026-11-01 capture, Claude outputs/plans/g923-capture-results.md).
    std::vector<std::uint8_t> reply(kVeryLongSize, 0);
    std::copy_n(report.begin(), std::min<std::size_t>(4, report.size()), reply.begin());
    reply[0] = kVeryLongId;
    const std::uint8_t featureIndex = report[2];
    const std::uint8_t function = static_cast<std::uint8_t>(report[3] >> 4U);
    const Params in = report.subspan(4);
    std::lock_guard lock(mutex_);
    ++requests_;
    nowMs_ = nowMs;
    softwareId_ = static_cast<std::uint8_t>(report[3] & 0x0FU);
    if (log_) log_(LogLevel::Debug, "hid++ in:  " + hex(report));
    if (featureIndex >= 0x80 && featureIndex <= 0x83) {
        // HID++ 1.0 register access: this wheel speaks 2.0 only. 1.0's error says so.
        note(featureIndex, 0, "a HID++ 1.0 register request (sub id " + std::to_string(featureIndex) +
                                  "): answered 'invalid sub id' (this wheel speaks 2.0)");
        reply[2] = 0x8F;
        reply[3] = featureIndex;
        reply[4] = report.size() > 3 ? report[3] : 0;
        reply[5] = 0x01;
    } else if (const auto error = answer(featureIndex, function, in, std::span(reply).subspan(4))) {
        std::fill(reply.begin() + 2, reply.end(), 0);
        reply[2] = 0xFF;
        reply[3] = featureIndex;
        reply[4] = report[3];
        reply[5] = *error;
    }
    if (log_) log_(LogLevel::Debug, "hid++ out: " + hex(reply));
    return reply;
}

std::optional<std::uint8_t> HidPp::answer(const std::uint8_t featureIndex, const std::uint8_t function, const Params in,
                                          const Out out) {
    if (featureIndex >= features_.size()) {
        note(featureIndex, function, "a request to feature index " + std::to_string(featureIndex) +
                                         ", which this wheel does not have: 'invalid feature index'");
        return kErrInvalidFeatureIndex;
    }
    switch (features_[featureIndex].id) {
        case kRoot: return root(function, in, out);
        case kFeatureSet: return featureSet(function, in, out);
        case kFwVersion: return fwVersion(function, in, out);
        case kNameType: return nameType(function, in, out);
        case kForceFeedback: return forceFeedback(function, in, out);
        default: return kErrInvalidFeatureIndex;
    }
}

std::optional<std::uint8_t> HidPp::root(const std::uint8_t function, const Params in, const Out out) {
    switch (function) {
        case 0: {   // getFeature(feature id) -> index, type, version
            const std::uint16_t id = be16(in, 0);
            for (std::size_t i = 0; i < features_.size(); ++i) {
                if (features_[i].id == id) {
                    out[0] = static_cast<std::uint8_t>(i);
                    out[1] = 0;   // type: a plain feature (not obsolete, hidden or engineering)
                    out[2] = features_[i].version;
                    note(0, static_cast<std::uint8_t>(0x80 + i), "asked for " + featureName(id) + ": index " +
                                                                    std::to_string(i));
                    return std::nullopt;
                }
            }
            if (askedFor_.insert(id).second && log_) {
                log_(LogLevel::Info, "hid++: asked for feature " + featureName(id) + ": not there (index 0)");
            }
            return std::nullopt;   // index 0: not there
        }
        case 1:   // getProtocolVersion(_, _, ping) -> 4.2, ping
            note(0, 1, "protocol version asked (ping): 4.2");
            out[0] = 4;
            out[1] = 2;
            out[2] = in.size() > 2 ? in[2] : 0;
            return std::nullopt;
        default:
            note(0, function, "Root function " + std::to_string(function) + ": 'invalid function'");
            return kErrInvalidFunction;
    }
}

std::optional<std::uint8_t> HidPp::featureSet(const std::uint8_t function, const Params in, const Out out) const {
    switch (function) {
        case 0:   // getCount (Root not counted)
            out[0] = static_cast<std::uint8_t>(features_.size() - 1);
            return std::nullopt;
        case 1: {   // getFeatureId(index) -> id, type, version
            const std::size_t index = in.empty() ? 0 : in[0];
            if (index >= features_.size()) return kErrOutOfRange;
            putBe16(out, 0, features_[index].id);
            out[2] = 0;
            out[3] = features_[index].version;
            return std::nullopt;
        }
        default: return kErrInvalidFunction;
    }
}

std::optional<std::uint8_t> HidPp::fwVersion(const std::uint8_t function, const Params in, const Out out) const {
    switch (function) {
        case 0:   // getEntityCount -> count, unit id, transport, model id, extended model id, capabilities
            out[0] = 1;
            out[1] = static_cast<std::uint8_t>(identity_.unitId >> 24U);
            out[2] = static_cast<std::uint8_t>(identity_.unitId >> 16U);
            out[3] = static_cast<std::uint8_t>(identity_.unitId >> 8U);
            out[4] = static_cast<std::uint8_t>(identity_.unitId);
            putBe16(out, 5, 0x0008);   // transport: USB
            putBe16(out, 7, identity_.productId);   // the model id: its USB product id first
            return std::nullopt;
        case 1: {   // getFwInfo(entity) -> type, prefix, number, revision, build, active, transport pid
            if (!in.empty() && in[0] != 0) return kErrOutOfRange;
            out[0] = 0;   // the main application
            out[1] = 'W';   // "WBR": WheelBri's, not Logitech's (high numbers: no update looks newer)
            out[2] = 'B';
            out[3] = 'R';
            out[4] = 0x99;   // number (BCD)
            out[5] = 0x99;   // revision (BCD)
            putBe16(out, 6, 0x9999);   // build (BCD)
            out[8] = 1;   // active
            putBe16(out, 9, identity_.productId);
            return std::nullopt;
        }
        default: return kErrInvalidFunction;
    }
}

std::optional<std::uint8_t> HidPp::nameType(const std::uint8_t function, const Params in, const Out out) const {
    switch (function) {
        case 0:   // getCount: the name's length
            out[0] = static_cast<std::uint8_t>(std::min<std::size_t>(identity_.name.size(), 255));
            return std::nullopt;
        case 1: {   // getDeviceName(char index): as much as fits from there
            const std::size_t from = in.empty() ? 0 : in[0];
            if (from > identity_.name.size()) return kErrOutOfRange;
            const std::size_t n = std::min(identity_.name.size() - from, out.size());
            std::copy_n(identity_.name.begin() + static_cast<std::ptrdiff_t>(from), n, out.begin());
            return std::nullopt;
        }
        case 2:   // getDeviceType
            out[0] = identity_.deviceType;
            return std::nullopt;
        default: return kErrInvalidFunction;
    }
}

std::optional<std::uint8_t> HidPp::forceFeedback(const std::uint8_t function, const Params in, const Out out) {
    const std::uint8_t index = 4;
    switch (function) {
        case 0:   // info: the number of effect slots
            note(index, function, "force feedback: info asked (" + std::to_string(kEffectSlots) + " slots)");
            out[0] = kEffectSlots;
            return std::nullopt;
        case 1:   // reset all
            note(index, function, "force feedback: reset all");
            nextSlot_ = 1;
            if (ffb_ != nullptr) ffb_->resetAll(nowMs_, softwareId_);
            return std::nullopt;
        case 2:   // download an effect: answered with its slot
            showCall(function, "effect downloaded", in);
            if (ffb_ != nullptr) {
                out[0] = ffb_->download(in, nowMs_, softwareId_);
                return out[0] == 0 ? std::optional<std::uint8_t>(kErrOutOfRange) : std::nullopt;
            }
            out[0] = nextSlot_;
            nextSlot_ = static_cast<std::uint8_t>(nextSlot_ % (kEffectSlots - 1) + 1);
            return std::nullopt;
        case 3:   // an effect's state (play, stop, pause): [0] slot [1] state
            showCall(function, "effect state", in.first(std::min<std::size_t>(2, in.size())));
            if (ffb_ != nullptr && in.size() >= 2) ffb_->setState(in[0], in[1], nowMs_, softwareId_);
            return std::nullopt;
        case 4:   // destroy an effect: [0] slot
            showCall(function, "effect freed", in.first(std::min<std::size_t>(1, in.size())));
            if (ffb_ != nullptr && !in.empty()) ffb_->destroy(in[0], nowMs_, softwareId_);
            return std::nullopt;
        case 5:   // get aperture: the rotation in degrees, the rig's own once known
            putBe16(out, 0, static_cast<std::uint16_t>(wheelRotationDeg_ > 0 ? wheelRotationDeg_ : rotationDeg_));
            return std::nullopt;
        case 6: {   // set aperture
            const int degrees = be16(in, 0);
            if (degrees < 40 || degrees > 2520) return kErrInvalidArgument;
            rotationDeg_ = degrees;
            if (log_) log_(LogLevel::Info, "hid++: rotation set to " + std::to_string(degrees) + " deg (not applied)");
            if (settings_) settings_(rotationDeg_, static_cast<float>(gain_) / 65535.0F);
            return std::nullopt;
        }
        case 7:   // get global gains
            putBe16(out, 0, gain_);
            putBe16(out, 2, boost_);
            return std::nullopt;
        case 8:   // set global gains
            gain_ = be16(in, 0);
            boost_ = be16(in, 2);
            if (log_) {
                const int percent = static_cast<int>(static_cast<float>(gain_) * 100.0F / 65535.0F + 0.5F);
                log_(LogLevel::Info, "hid++: force feedback gain set to " + std::to_string(percent) + "% (boost " +
                                         std::to_string(boost_) + "; not applied)");
            }
            if (ffb_ != nullptr) ffb_->setGain(static_cast<float>(gain_) / 65535.0F, nowMs_);
            if (settings_) settings_(rotationDeg_, static_cast<float>(gain_) / 65535.0F);
            return std::nullopt;
        default:
            note(index, function, "force feedback function " + std::to_string(function) + ": 'invalid function'");
            return kErrInvalidFunction;
    }
}

}  // namespace wheelbri
