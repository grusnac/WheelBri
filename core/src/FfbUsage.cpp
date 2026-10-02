#include "wheelemu/FfbUsage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wheelemu {

namespace {

constexpr std::uint64_t kMaxDtNs = 100'000'000;
constexpr float kEpsilon = 1e-4F;

// Did the safety cap (or, for spring and co., the coefficient range) cut the game's effect down?
// For spring, damper, friction and inertia the cap is a ceiling on the force they can reach, which
// depends on where the wheel is; only coefficients beyond the range count here.
bool capped(const FfbEffect& requested, const FfbSafetyConfig& safety) {
    const float gain = requested.gain * safety.masterGain;
    if (requested.type == FfbEffectType::Constant) {
        return std::abs(requested.constantLevel * gain) > safety.maxMagnitude + kEpsilon;
    }
    if (isConditionEffect(requested.type)) {
        return std::abs(requested.condition.positiveCoefficient * gain) > 1.0F + kEpsilon ||
               std::abs(requested.condition.negativeCoefficient * gain) > 1.0F + kEpsilon;
    }
    const float magnitude = requested.periodic.magnitude * gain;
    const float offsetCap = std::max(safety.maxMagnitude - std::min(magnitude, safety.maxMagnitude), 0.0F);
    return magnitude > safety.maxMagnitude + kEpsilon || std::abs(requested.periodic.offset * gain) > offsetCap + kEpsilon;
}

// Constant force held back by the slew limit: the wheel is not (yet) where the cap would allow.
bool slewed(const FfbEffect& requested, const FfbEffect& sent, const FfbSafetyConfig& safety) {
    if (requested.type != FfbEffectType::Constant) {
        return false;
    }
    const float target = std::clamp(requested.constantLevel * requested.gain * safety.masterGain,
                                    -safety.maxMagnitude, safety.maxMagnitude);
    return std::abs(sent.constantLevel - target) > 0.005F;
}

std::string percent(const double share) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", std::clamp(share, 0.0, 1.0) * 100.0);
    return text;
}

std::string duration(const std::uint64_t ns) {
    const std::uint64_t seconds = ns / 1'000'000'000ULL;
    char text[24];
    std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>(seconds / 60 % 60), static_cast<unsigned long long>(seconds % 60));
    return text;
}

}  // namespace

float FfbUsage::TypeStats::askedAverage() const {
    return playNs == 0 ? 0.0F : static_cast<float>(askedStrengthNs / static_cast<double>(playNs));
}

float FfbUsage::TypeStats::wheelAverage() const {
    return playNs == 0 ? 0.0F : static_cast<float>(wheelStrengthNs / static_cast<double>(playNs));
}

FfbUsage::FfbUsage(const FfbSafetyConfig& safety) : safety_(safety) {
    safety_.masterGain = std::clamp(safety_.masterGain, 0.0F, 1.0F);
    safety_.maxMagnitude = std::clamp(safety_.maxMagnitude, 0.0F, 1.0F);
}

void FfbUsage::record(std::uint64_t dtNs, const bool gameActive, const std::span<const SlotSample> slots) {
    dtNs = std::min(dtNs, kMaxDtNs);

    // Per type this tick: several slots of one type count once, at the strongest.
    struct Tick {
        bool playing = false;
        float asked = 0.0F;
        float wheel = 0.0F;
        bool capped = false;
        bool slewed = false;
    };
    std::array<Tick, kTypes> now{};
    bool any = false;
    float constantAsked = -1.0F;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const SlotSample& slot = slots[i];
        Previous* const previous = i < previous_.size() ? &previous_[i] : nullptr;
        if (!slot.playing) {
            if (previous != nullptr) {
                previous->playing = false;
            }
            continue;
        }
        const FfbEffectType type = slot.requested.type;
        const auto index = static_cast<std::size_t>(type);
        if (index >= kTypes) {
            continue;
        }
        if (previous != nullptr && (!previous->playing || previous->type != type)) {
            ++types_[index].starts;
        }
        if (previous != nullptr) {
            *previous = {true, type};
        }
        any = true;
        Tick& tick = now[index];
        const float asked = std::min(std::abs(ffbStrength(slot.requested)), 1.0F);
        tick.playing = true;
        tick.asked = std::max(tick.asked, asked);
        tick.wheel = std::max(tick.wheel, std::min(std::abs(ffbStrength(slot.sent)), 1.0F));
        tick.capped = tick.capped || capped(slot.requested, safety_);
        tick.slewed = tick.slewed || slewed(slot.requested, slot.sent, safety_);
        if (type == FfbEffectType::Constant) {
            constantAsked = std::max(constantAsked, asked);
        }
    }

    if (gameActive) {
        gameNs_ += dtNs;
    }
    if (any) {
        anyForceNs_ += dtNs;
    }
    for (std::size_t t = 0; t < kTypes; ++t) {
        const Tick& tick = now[t];
        if (!tick.playing) {
            continue;
        }
        TypeStats& stats = types_[t];
        stats.playNs += dtNs;
        stats.askedStrengthNs += static_cast<double>(tick.asked) * static_cast<double>(dtNs);
        stats.wheelStrengthNs += static_cast<double>(tick.wheel) * static_cast<double>(dtNs);
        stats.askedPeak = std::max(stats.askedPeak, tick.asked);
        stats.wheelPeak = std::max(stats.wheelPeak, tick.wheel);
        if (tick.capped) {
            stats.cappedNs += dtNs;
        }
        if (tick.slewed) {
            stats.slewNs += dtNs;
        }
    }
    if (constantAsked >= 0.0F) {
        const auto bin = std::min(static_cast<std::size_t>(constantAsked * static_cast<float>(kBins)), kBins - 1);
        histogramNs_[bin] += dtNs;
    }
}

std::vector<FfbEffectType> FfbUsage::usedTypes() const {
    std::vector<FfbEffectType> used;
    for (std::size_t t = 0; t < kTypes; ++t) {
        if (types_[t].starts > 0 || types_[t].playNs > 0) {
            used.push_back(static_cast<FfbEffectType>(t));
        }
    }
    std::stable_sort(used.begin(), used.end(),
                     [this](const FfbEffectType a, const FfbEffectType b) { return type(a).playNs > type(b).playNs; });
    return used;
}

std::string FfbUsage::tableHeader() {
    char line[128];
    std::snprintf(line, sizeof(line), "%-8s %6s %8s %5s %11s %12s %6s %5s", "type", "starts", "time", "share",
                  "game avg/pk", "wheel avg/pk", "capped", "slew");
    return line;
}

std::string FfbUsage::tableRow(const FfbEffectType t) const {
    const TypeStats& s = type(t);
    const double played = static_cast<double>(s.playNs);
    const double share = gameNs_ == 0 ? 0.0 : played / static_cast<double>(gameNs_);
    const std::string capped = s.playNs == 0 ? "-" : percent(static_cast<double>(s.cappedNs) / played);
    const std::string slew = t != FfbEffectType::Constant || s.playNs == 0
                                 ? "-"
                                 : percent(static_cast<double>(s.slewNs) / played);
    char line[160];
    const std::string asked = percent(s.askedAverage()) + " / " + percent(s.askedPeak);
    const std::string wheel = percent(s.wheelAverage()) + " / " + percent(s.wheelPeak);
    std::snprintf(line, sizeof(line), "%-8s %6llu %8s %5s %11s %12s %6s %5s", ffbTypeName(t),
                  static_cast<unsigned long long>(s.starts), duration(s.playNs).c_str(), percent(share).c_str(),
                  asked.c_str(), wheel.c_str(), capped.c_str(), slew.c_str());
    return line;
}

std::string FfbUsage::histogramLine() const {
    std::uint64_t total = 0;
    for (const auto ns : histogramNs_) {
        total += ns;
    }
    if (total == 0) {
        return "constant force strength the game asked for: none yet";
    }
    std::string line = "constant force strength the game asked for (share of its time):";
    for (std::size_t i = 0; i < kBins; ++i) {
        char part[32];
        std::snprintf(part, sizeof(part), " %zu-%zu%% %s", i * 10, (i + 1) * 10,
                      percent(static_cast<double>(histogramNs_[i]) / static_cast<double>(total)).c_str());
        line += part;
        if (i + 1 < kBins) {
            line += ",";
        }
    }
    return line;
}

}  // namespace wheelemu
