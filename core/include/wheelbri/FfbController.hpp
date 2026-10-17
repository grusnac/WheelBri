#pragma once

#include "wheelbri/Devices.hpp"
#include "wheelbri/Ffb.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FfbUsage.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace wheelbri {

// Service side of game force feedback: keeps the slots the game asked for (FfbPackets from the shim),
// passes every effect through its own FfbSafetyLimiter, and plays the result on the physical wheel.
//
// Safety rules:
//  * nothing reaches the sink without the limiter;
//  * if the shim goes quiet for `watchdogNs` (game crashed, paused, or lost the device) every effect is
//    stopped, and stays stopped until the game sends it again;
//  * a slot is re-sent to the sink at most every `minUpdateNs` unless it starts or stops, so a slewing
//    constant force cannot flood the driver;
//  * a constant push the game leaves exactly as it is for `frozenForceSeconds` (FfbSafetyConfig) fades out over
//    `kFrozenFadeNs`, with every periodic effect on top: a game on a loading screen that keeps sending its last
//    force (NFS 2015: 83% and a rumble for 21 s). Springs and dampers stay. Any change brings the forces back.
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

    // A game profile's force feedback gain ([ffb] gain, GameProfiles.hpp): the game's pushing forces (constant force
    // level, periodic magnitude and offset) times this before the limiter; conditions are left as they are. 1 = as
    // the game asks; clamped to kFfbGainMin..kFfbGainMax. Takes effect at the next update().
    void setGameGain(float gain);
    [[nodiscard]] float gameGain() const { return gameGain_; }
    // `effect` with the game gain applied, as update() hands it to the limiter.
    [[nodiscard]] static FfbEffect withGameGain(const FfbEffect& effect, float gain);

    [[nodiscard]] const Stats& stats() const { return stats_; }

    // How long a frozen push takes to fade out, and the smallest push (of full scale) that counts as one.
    static constexpr std::uint64_t kFrozenFadeNs = 500'000'000;
    static constexpr float kFrozenPushAbove = 0.05F;
    // What the game's pushing forces are multiplied by now: 1, down to 0 while a frozen push fades (last update()).
    [[nodiscard]] float frozenFade() const { return frozenFade_; }

    // Read-only view of one slot, for displays: what the game asked for and what reached the wheel.
    struct SlotView {
        bool wanted = false;                 // the game wants it playing
        bool loaded = false;                 // the game has sent this slot's effect since its last "stop all"
        bool playing = false;                // the sink is playing it
        FfbEffect requested;                 // as the game sent it (before the limiter)
        std::optional<FfbEffect> sent;       // last effect given to the sink (after the limiter)
    };
    [[nodiscard]] SlotView slot(std::size_t index) const {
        if (index >= slots_.size()) return {};   // no such slot: nothing wanted or playing
        const Slot& s = slots_[index];
        return {s.wanted, s.loaded, s.playing, s.requested, s.lastSent};
    }
    [[nodiscard]] std::size_t slotCount() const { return slots_.size(); }

    // Lasting effects (constant force, spring, damper, friction, inertia: what a game holds for the whole race) the
    // game has loaded, and how many of those it wants playing. For FfbIdleWatch.
    [[nodiscard]] static bool lasting(FfbEffectType type);
    [[nodiscard]] std::size_t lastingLoaded() const;
    [[nodiscard]] std::size_t lastingWanted() const;
    [[nodiscard]] std::optional<std::uint64_t> lastPacketNs() const { return lastPacketNs_; }
    [[nodiscard]] bool watchdogTripped() const { return watchdogTripped_; }
    [[nodiscard]] const FfbSafetyConfig& safety() const { return safety_; }

    // Which effects the game used and how hard, since the start (updated every update() call).
    [[nodiscard]] const FfbUsage& usage() const { return usage_; }

private:
    struct Slot {
        bool wanted = false;                 // the game wants this slot playing
        bool loaded = false;                 // a SlotState came since the last StopAll
        FfbEffect requested;
        bool playing = false;                // what the sink is doing
        std::optional<FfbEffect> lastSent;
        std::uint64_t lastSentNs = 0;
        std::uint64_t lastUpdateNs = 0;      // for the limiter's slew step
        std::uint64_t pushSinceNs = 0;       // when the game last changed this slot's push (type, level, gain)
    };

    // The fade for frozen pushes at `nowNs` (1 = none), logging when it starts and ends.
    [[nodiscard]] float frozenFadeAt(std::uint64_t nowNs);

    [[nodiscard]] static bool differs(const FfbEffect& a, const FfbEffect& b);

    IFfbSink& sink_;
    FfbSafetyConfig safety_;
    Timing timing_;
    std::array<Slot, IFfbSink::kMaxSlots> slots_{};
    // One per slot; an array, so copying the controller (the window's frame, 60 times a second) allocates nothing.
    std::array<FfbSafetyLimiter, IFfbSink::kMaxSlots> limiters_;
    std::optional<std::uint64_t> lastPacketNs_;
    float gameGain_ = 1.0F;
    std::uint64_t frozenNs_ = 0;            // from safety_.frozenForceSeconds; 0 = never
    float frozenFade_ = 1.0F;
    bool watchdogTripped_ = false;
    Stats stats_;
    FfbUsage usage_;
    std::optional<std::uint64_t> lastUsageNs_;
};

}  // namespace wheelbri
