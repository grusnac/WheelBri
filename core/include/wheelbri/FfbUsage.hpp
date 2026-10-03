#pragma once

#include "wheelbri/Devices.hpp"
#include "wheelbri/Ffb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wheelbri {

// Which parts of force feedback a game uses, since the service started: per effect type how often it
// started, how long it played, how strong the game asked for it and what reached the wheel, and how long
// the safety limits cut it down. Plus, for the constant force (road and tyre feel), how much time was spent
// at each strength. Fed by FfbController every loop tick; shown in the --ui window and in the log.
class FfbUsage {
public:
    static constexpr std::size_t kTypes = 10;      // FfbEffectType values
    static constexpr std::size_t kBins = 10;       // constant force histogram: 0-10%, 10-20%, ... 90-100%

    struct TypeStats {
        std::uint64_t starts = 0;
        std::uint64_t playNs = 0;          // time at least one effect of this type was playing
        double askedStrengthNs = 0.0;      // game's strength (0..1) x time, for the average
        float askedPeak = 0.0F;
        double wheelStrengthNs = 0.0;      // after gain and limits, share of the wheel's full torque
        float wheelPeak = 0.0F;
        std::uint64_t cappedNs = 0;        // the safety cap (max_magnitude) cut it down
        std::uint64_t slewNs = 0;          // constant only: the slew limit held it back

        [[nodiscard]] float askedAverage() const;
        [[nodiscard]] float wheelAverage() const;
    };

    // One slot, as the controller sees it this tick.
    struct SlotSample {
        bool playing = false;
        FfbEffect requested;               // as the game sent it
        FfbEffect sent;                    // what the wheel got (valid when playing)
    };

    explicit FfbUsage(const FfbSafetyConfig& safety);

    // `dtNs` since the last call (capped at 100 ms, so a sleeping PC does not count as play time);
    // `gameActive`: the game is sending force feedback (it has, and has not gone quiet).
    void record(std::uint64_t dtNs, bool gameActive, std::span<const SlotSample> slots);

    [[nodiscard]] const TypeStats& type(FfbEffectType t) const { return types_[static_cast<std::size_t>(t)]; }
    [[nodiscard]] std::uint64_t gameNs() const { return gameNs_; }          // time the game was sending
    [[nodiscard]] std::uint64_t anyForceNs() const { return anyForceNs_; }  // time any effect was playing
    [[nodiscard]] const std::array<std::uint64_t, kBins>& constantHistogramNs() const { return histogramNs_; }
    [[nodiscard]] bool empty() const { return anyForceNs_ == 0; }

    // Effect types that played, the longest first.
    [[nodiscard]] std::vector<FfbEffectType> usedTypes() const;

    // Text table for the log and the window (68 columns):
    //   type     starts     time share game avg/pk wheel avg/pk capped  slew
    //   constant    412  0:41:22   78%   38% / 90%    30% / 80%    12%    4%
    [[nodiscard]] static std::string tableHeader();
    [[nodiscard]] std::string tableRow(FfbEffectType t) const;
    // "constant force asked: 0-10% 12%, 10-20% 30%, ..." (share of constant force time per strength).
    [[nodiscard]] std::string histogramLine() const;

private:
    FfbSafetyConfig safety_;               // clamped like the limiter does
    std::array<TypeStats, kTypes> types_{};
    std::array<std::uint64_t, kBins> histogramNs_{};
    std::uint64_t gameNs_ = 0;
    std::uint64_t anyForceNs_ = 0;
    struct Previous {
        bool playing = false;
        FfbEffectType type = FfbEffectType::Constant;
    };
    std::array<Previous, IFfbSink::kMaxSlots> previous_{};
};

}  // namespace wheelbri
