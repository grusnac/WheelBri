#pragma once

#include "wheelemu/Devices.hpp"
#include "wheelemu/Ffb.hpp"
#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/FfbUsage.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wheelemu {

// Service side of game force feedback: keeps the slots the game asked for (FfbPackets from the shim),
// passes every effect through its own FfbSafetyLimiter, and plays the result on the physical wheel.
//
// Safety rules:
//  * nothing reaches the sink without the limiter;
//  * if the shim goes quiet for `watchdogNs` (game crashed, paused, or lost the device) every effect is
//    stopped, and stays stopped until the game sends it again;
//  * a slot is re-sent to the sink at most every `minUpdateNs` unless it starts or stops, so a slewing
//    constant force cannot flood the driver.
class FfbController {
public:
    struct Timing {
        std::uint64_t watchdogNs = 500'000'000;
        std::uint64_t minUpdateNs = 4'000'000;
    };

    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t plays = 0;             // sink.play calls
        std::uint64_t playFailures = 0;
        std::uint64_t watchdogStops = 0;
        std::size_t activeSlots = 0;
    };

    FfbController(IFfbSink& sink, const FfbSafetyConfig& safety, Timing timing);
    FfbController(IFfbSink& sink, const FfbSafetyConfig& safety) : FfbController(sink, safety, Timing{}) {}

    void onPacket(const FfbPacket& packet, std::uint64_t nowNs);

    // Call regularly (the service does it every loop tick): applies limits, updates the sink, watchdog.
    void update(std::uint64_t nowNs);

    // Stops everything now (service shutting down).
    void stopAll();

    [[nodiscard]] const Stats& stats() const { return stats_; }

    // Read-only view of one slot, for displays: what the game asked for and what reached the wheel.
    struct SlotView {
        bool wanted = false;                 // the game wants it playing
        bool playing = false;                // the sink is playing it
        FfbEffect requested;                 // as the game sent it (before the limiter)
        std::optional<FfbEffect> sent;       // last effect given to the sink (after the limiter)
    };
    [[nodiscard]] SlotView slot(std::size_t index) const {
        const Slot& s = slots_[index];
        return {s.wanted, s.playing, s.requested, s.lastSent};
    }
    [[nodiscard]] std::size_t slotCount() const { return slots_.size(); }
    [[nodiscard]] std::optional<std::uint64_t> lastPacketNs() const { return lastPacketNs_; }
    [[nodiscard]] bool watchdogTripped() const { return watchdogTripped_; }
    [[nodiscard]] const FfbSafetyConfig& safety() const { return safety_; }

    // Which effects the game used and how hard, since the start (updated every update() call).
    [[nodiscard]] const FfbUsage& usage() const { return usage_; }

private:
    struct Slot {
        bool wanted = false;                 // the game wants this slot playing
        FfbEffect requested;
        bool playing = false;                // what the sink is doing
        std::optional<FfbEffect> lastSent;
        std::uint64_t lastSentNs = 0;
        std::uint64_t lastUpdateNs = 0;      // for the limiter's slew step
    };

    [[nodiscard]] static bool differs(const FfbEffect& a, const FfbEffect& b);

    IFfbSink& sink_;
    FfbSafetyConfig safety_;
    Timing timing_;
    std::array<Slot, IFfbSink::kMaxSlots> slots_{};
    std::vector<FfbSafetyLimiter> limiters_;
    std::optional<std::uint64_t> lastPacketNs_;
    bool watchdogTripped_ = false;
    Stats stats_;
    FfbUsage usage_;
    std::optional<std::uint64_t> lastUsageNs_;
};

}  // namespace wheelemu
