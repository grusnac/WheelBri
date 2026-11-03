#include "wheelbri/ClassicFfb.hpp"

#include "wheelbri/HidPid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>

namespace wheelbri {

namespace {

constexpr std::uint8_t kDownload = 0x00;
constexpr std::uint8_t kDownloadAndPlay = 0x01;
constexpr std::uint8_t kPlay = 0x02;
constexpr std::uint8_t kStop = 0x03;
constexpr std::uint8_t kSpringOn = 0x04;
constexpr std::uint8_t kSpringOff = 0x05;
constexpr std::uint8_t kNormalOrExtended = 0x08;
constexpr std::uint8_t kRefresh = 0x0C;
constexpr std::uint8_t kSetDefaultSpring = 0x0E;

// A 3-bit spring or damper constant (Table 27: the G25's and G27's): 1/4 .. 4 times the offset, as a quarter of that
// so that the strongest is 1.
constexpr float kLowResK[8] = {0.25F, 0.5F, 0.75F, 1.0F, 1.5F, 2.0F, 3.0F, 4.0F};

float lowResK(const std::uint8_t k) { return kLowResK[k & 7U] / 4.0F; }
float highResK(const std::uint8_t k) { return static_cast<float>(k & 0x0FU) / 15.0F; }
float unit(const std::uint8_t byte) { return static_cast<float>(byte) / 255.0F; }
// A wheel position 0..255 as -1..1.
float position(const float counts) { return std::clamp((counts - 127.5F) / 127.5F, -1.0F, 1.0F); }

// Not Ffb.cpp's helpers: the driver compiles this file without the rest of core (as HidPpFfb).
bool isCondition(const FfbEffectType type) {
    return type == FfbEffectType::Spring || type == FfbEffectType::Damper || type == FfbEffectType::Friction ||
           type == FfbEffectType::Inertia;
}

const char* typeName(const std::uint8_t type) {
    constexpr const char* kNames[] = {"constant", "spring", "damper", "auto-centring spring", "sawtooth up",
                                      "sawtooth down", "trapezoid", "rectangle", "variable", "ramp", "square wave",
                                      "high-res spring", "high-res damper", "high-res auto-centring spring",
                                      "friction"};
    return type < std::size(kNames) ? kNames[type] : "unknown";
}

// A condition: K1 the left (negative) side, K2 the right; S1/S2 inverted.
FfbEffect condition(const FfbEffectType type, const float k1, const bool s1, const float k2, const bool s2,
                    const float clip) {
    FfbEffect e;
    e.type = type;
    e.condition.negativeCoefficient = s1 ? -k1 : k1;
    e.condition.positiveCoefficient = s2 ? -k2 : k2;
    e.condition.negativeSaturation = clip;
    e.condition.positiveSaturation = clip;
    return e;
}

std::string hex(const std::array<std::uint8_t, ClassicFfb::kReportSize>& r) {
    std::string text;
    for (const std::uint8_t b : r) {
        char two[4];
        std::snprintf(two, sizeof(two), "%s%02x", text.empty() ? "" : " ", static_cast<unsigned>(b));
        text += two;
    }
    return text;
}

std::string describe(const FfbEffect& e) {
    char text[160];
    if (e.type == FfbEffectType::Constant) {
        std::snprintf(text, sizeof(text), "constant %.3f", e.constantLevel);
    } else {
        std::snprintf(text, sizeof(text), "%s k %.2f/%.2f sat %.2f deadband %.3f centre %.3f",
                      e.type == FfbEffectType::Spring    ? "spring"
                      : e.type == FfbEffectType::Damper ? "damper"
                                                        : "friction",
                      e.condition.negativeCoefficient, e.condition.positiveCoefficient,
                      e.condition.positiveSaturation, e.condition.deadband, e.condition.center);
    }
    return text;
}

}  // namespace

ClassicFfb::ClassicFfb(Sender send, Logger log) : send_(std::move(send)), log_(std::move(log)) {}

float ClassicFfb::level(const std::uint8_t byte) {
    return std::clamp((static_cast<float>(byte) - 127.5F) / 127.5F, -1.0F, 1.0F);
}

std::optional<FfbEffect> ClassicFfb::decode(const std::array<std::uint8_t, kReportSize>& r, const std::size_t slot) {
    const std::uint8_t type = r[1];
    switch (type) {
        case 0x00: {   // constant: one level per slot
            FfbEffect e;
            e.type = FfbEffectType::Constant;
            e.constantLevel = level(r[2 + slot]);
            return e;
        }
        case 0x08: {   // variable: F0's level in byte 2, F2's in byte 3 (only those two)
            if (slot != 0 && slot != 2) return std::nullopt;
            FfbEffect e;
            e.type = FfbEffectType::Constant;
            e.constantLevel = level(slot == 0 ? r[2] : r[3]);
            return e;
        }
        case 0x01:
        case 0x0B: {   // spring: dead band D1..D2, K2 (high nibble) right, K1 (low) left, S2 bit 4, S1 bit 0, CLIP
            const bool high = type == 0x0B;
            const std::uint8_t k1 = r[4] & (high ? 0x0FU : 0x07U);
            const std::uint8_t k2 = static_cast<std::uint8_t>((r[4] >> 4) & (high ? 0x0FU : 0x07U));
            FfbEffect e = condition(FfbEffectType::Spring, high ? highResK(k1) : lowResK(k1), (r[5] & 0x01U) != 0,
                                    high ? highResK(k2) : lowResK(k2), (r[5] & 0x10U) != 0, unit(r[6]));
            const float d1 = r[2];
            const float d2 = std::max<float>(r[2], r[3]);
            e.condition.center = position((d1 + d2) / 2.0F);
            e.condition.deadband = (d2 - d1) / 255.0F;
            return e;
        }
        case 0x02:
        case 0x0C: {   // damper: K1, S1, K2, S2 (byte 6 a clip on a Driving Force Pro only: full here)
            const bool high = type == 0x0C;
            return condition(FfbEffectType::Damper, high ? highResK(r[2]) : lowResK(r[2]), (r[3] & 0x01U) != 0,
                             high ? highResK(r[4]) : lowResK(r[4]), (r[5] & 0x01U) != 0, 1.0F);
        }
        case 0x03:
        case 0x0D: {   // auto-centring spring: centred, dead band one count each side
            const bool high = type == 0x0D;
            FfbEffect e = condition(FfbEffectType::Spring, high ? highResK(r[2]) : lowResK(r[2]), false,
                                    high ? highResK(r[3]) : lowResK(r[3]), false, unit(r[4]));
            e.condition.deadband = 1.0F / 127.5F;
            return e;
        }
        case 0x0E:   // friction: K1, K2 0..255, CLIP, S2 bit 4, S1 bit 0
            return condition(FfbEffectType::Friction, unit(r[2]), (r[5] & 0x01U) != 0, unit(r[3]),
                             (r[5] & 0x10U) != 0, unit(r[4]));
        default:
            return std::nullopt;
    }
}

void ClassicFfb::request(const std::int64_t nowMs) {
    if (requests_ == 0 || nowMs - lastRequestMs_ > HidPid::kStreamGapMs) streamStartMs_ = nowMs;
    ++requests_;
    lastRequestMs_ = nowMs;
    if (quiet_) {
        quiet_ = false;
        if (log_) log_(LogLevel::Info, "classic ffb: the game sends again: keep-alives back");
    }
}

void ClassicFfb::note(const std::string& what) {
    if (!noted_.insert(what).second) return;
    if (log_) log_(LogLevel::Info, "classic ffb: " + what);
}

void ClassicFfb::command(const std::span<const std::uint8_t> bytes, const std::int64_t nowMs) {
    std::array<std::uint8_t, kReportSize> r{};
    std::copy_n(bytes.begin(), std::min(bytes.size(), r.size()), r.begin());
    std::lock_guard lock(mutex_);
    request(nowMs);
    const std::uint8_t cmd = r[0] & 0x0FU;
    const std::uint8_t mask = static_cast<std::uint8_t>(r[0] >> 4);
    if (log_) {
        const bool shown = shown_ < kShownCommands;
        if (shown) ++shown_;
        log_(shown ? LogLevel::Info : LogLevel::Debug, "classic ffb: " + hex(r));
    }
    if (cmd == kNormalOrExtended && mask == 0x0F) {
        char text[96];
        if (r[1] == 0x81) {
            std::snprintf(text, sizeof(text), "the driver set the wheel range to %u deg (not applied: the mapping's)",
                          static_cast<unsigned>(r[2] | (r[3] << 8)));
            if (log_) log_(LogLevel::Info, std::string("classic ffb: ") + text);
        } else {
            std::snprintf(text, sizeof(text), "extended command 0x%02x: not used", static_cast<unsigned>(r[1]));
            note(text);
        }
        return;
    }
    switch (cmd) {
        case kDownload:
        case kDownloadAndPlay:
        case kRefresh:
            for (std::size_t s = 0; s < kSlots; ++s) {
                if ((mask & (1U << s)) == 0) continue;
                const std::optional<FfbEffect> effect = decode(r, s);
                if (!effect) {
                    char text[96];
                    std::snprintf(text, sizeof(text), "force type %s (0x%02x): not played yet", typeName(r[1]),
                                  static_cast<unsigned>(r[1]));
                    note(text);
                    continue;
                }
                effectsSeen_ = true;
                slots_[s].effect = *effect;
                if (cmd != kDownload) slots_[s].playing = true;
            }
            break;
        case kPlay:
        case kStop:
            for (std::size_t s = 0; s < kSlots; ++s) {
                if ((mask & (1U << s)) == 0 || !slots_[s].effect) continue;
                slots_[s].playing = cmd == kPlay;
            }
            break;
        case kSpringOn:
        case kSpringOff:
        case kSetDefaultSpring:
            note("default spring commands: not played (the target's centring spring is WheelBri's)");
            break;
        default: {
            char text[64];
            std::snprintf(text, sizeof(text), "command 0x%02x: not used", static_cast<unsigned>(cmd));
            note(text);
            break;
        }
    }
    sendChanges(nowMs, false);
}

void ClassicFfb::sendChanges(const std::int64_t nowMs, const bool refresh) {
    for (std::size_t s = 0; s < kSlots; ++s) {
        const Slot& slot = slots_[s];
        if (!slot.effect) continue;
        const auto& last = sent_[s];
        const bool changed = !last || last->playing != slot.playing || (slot.playing && !(last->effect == *slot.effect));
        if (changed || (refresh && last && last->playing)) {
            if (log_ && changed && slot.playing && shown_ <= kShownCommands) {
                log_(LogLevel::Debug, "classic ffb: slot " + std::to_string(s) + " " + describe(*slot.effect));
            }
            send(FfbPacket::Kind::SlotState, static_cast<std::uint8_t>(s), slot.playing, *slot.effect, nowMs);
            sent_[s] = Sent{slot.playing, *slot.effect};
        }
    }
}

void ClassicFfb::send(const FfbPacket::Kind kind, const std::uint8_t slot, const bool playing,
                      const FfbEffect& effect, const std::int64_t nowMs) {
    FfbPacket packet;
    packet.kind = kind;
    packet.slot = slot;
    packet.sequence = ++sequence_;
    packet.playing = playing;
    packet.effect = effect;
    if (send_) send_(packet);
    lastSendMs_ = nowMs;
    ++packetsSent_;
}

bool ClassicFfb::pushing(const float above) const {
    for (const auto& last : sent_) {
        if (!last || !last->playing || isCondition(last->effect.type)) continue;
        if (std::fabs(last->effect.constantLevel) * last->effect.gain > above) return true;
    }
    return false;
}

void ClassicFfb::tick(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    if (!effectsSeen_) return;   // keep-alives only once an effect came
    const bool streamed = lastRequestMs_ - streamStartMs_ >= HidPid::kStreamMinMs;
    if (!quiet_ && streamed && nowMs - lastRequestMs_ >= HidPid::kStreamQuietMs &&
        pushing(HidPid::kStreamPushingAbove)) {
        quiet_ = true;
        if (log_) {
            log_(LogLevel::Info, "classic ffb: the game streamed force feedback and then stopped (closed, crashed or "
                                 "paused?): keep-alives stopped, so the service stops the forces");
        }
    }
    if (!quiet_ && nowMs - lastRequestMs_ >= HidPid::kGameQuietMs && pushing(HidPid::kQuietPushingAbove)) {
        quiet_ = true;
        if (log_) {
            log_(LogLevel::Warn, "classic ffb: nothing from the game for " +
                                     std::to_string(HidPid::kGameQuietMs / 1000) +
                                     " s while a pushing force plays: keep-alives stopped");
        }
    }
    const bool refresh = !quiet_ && nowMs - lastRefreshMs_ >= kRefreshMs;
    if (refresh) lastRefreshMs_ = nowMs;
    sendChanges(nowMs, refresh);
    if (!quiet_ && nowMs - lastSendMs_ >= kKeepAliveMs) send(FfbPacket::Kind::KeepAlive, 0, false, FfbEffect{}, nowMs);
}

void ClassicFfb::stopAll(const std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    for (Slot& s : slots_) s.playing = false;
    sent_.fill(std::nullopt);
    if (effectsSeen_) send(FfbPacket::Kind::StopAll, 0, false, FfbEffect{}, nowMs);
}

std::uint64_t ClassicFfb::packetsSent() const {
    std::lock_guard lock(mutex_);
    return packetsSent_;
}

}  // namespace wheelbri
