// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Devices.hpp"
#include "wheelbri/Ffb.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FfbUsage.hpp"
#include "wheelbri/GameTuning.hpp"

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
//  * a cap below full scale (FfbSafetyConfig::maxMagnitude < 1) holds for all slots together: when the constant
//    forces and the periodic effects' offsets and magnitudes add up to more, every one of them is scaled down by the
//    same factor;
//  * if the shim goes quiet for `watchdogNs` (game crashed, paused, or lost the device) every effect is
//    stopped, and stays stopped until the game sends it again;
//  * several senders at once (the virtual wheel's driver has HID PID, HID++ and TrueForce, each its own socket and
//    port; a shim; a game that restarted): each `source` has slots of its own (its slot n where it can, another free
//    one when another source holds n) and a watchdog of its own: one that goes quiet for `watchdogNs` has its forces
//    stopped even while another keeps sending, and its "stop all" stops only its own;
//  * a slot is re-sent to the sink at most every `minUpdateNs` unless it starts or stops, so a slewing
//    constant force cannot flood the driver;
//  * a constant push (or a periodic effect's offset) the game leaves exactly as it is for `frozenForceSeconds`
//    (FfbSafetyConfig) fades out over `kFrozenFadeNs`, with every periodic effect on top: a game on a loading screen
//    that keeps sending its last force (NFS 2015: 83% and a rumble for 21 s). Springs and dampers stay. Any change
//    brings the forces back;
//  * a constant force (or a periodic effect's offset) at exactly full scale (either way) while the game has sent no
//    push between none and full for `kRealForceWithinNs` is a "lone full push" and plays as none
//    (FfbSafetyConfig::ignoreLoneFullPush).
//    Race Driver: GRID (2008) sometimes passes lMagnitude = -2147483648 instead of its force (VSync off at 144 Hz or
//    more: 2026-10-19; at 300 Hz with VSync off: 2026-10-21, a stream of nothing but 0 and -100% for minutes). The shim
//    sees that number and drops it (DiEffect.hpp, isGarbage), but in Mode Virtual Windows' HID PID driver clamps it to
//    -10000 first, and the wheel's driver gets a full push like any other. A real stream has every value in between
//    (GRID with VSync on: 170,000 values in 40 minutes, 0.35% of them full, all within a moment of others), so a full
//    push that comes with them still plays.
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
        std::uint64_t loneFullPushes = 0;    // full pushes played as none (no real force around them)
        std::size_t activeSlots = 0;
    };

    FfbController(IFfbSink& sink, const FfbSafetyConfig& safety, Timing timing);
    FfbController(IFfbSink& sink, const FfbSafetyConfig& safety) : FfbController(sink, safety, Timing{}) {}

    // `source`: who sent it (the sender's UDP port; 0 for one that is not told apart). A source not heard of before
    // takes a place of its own (at most kMaxSources; the one quiet longest gives its place up).
    void onPacket(const FfbPacket& packet, std::uint64_t nowNs, std::uint32_t source = 0);
    static constexpr std::size_t kMaxSources = 4;

    // Call regularly (the service does it every loop tick): applies limits, updates the sink, watchdog.
    void update(std::uint64_t nowNs);

    // Stops everything now (service shutting down).
    void stopAll();

    // `effect` as update() hands it to the limiter: its pushing forces (constant level, periodic magnitude and
    // offset) times `pushFade` (1 = as the game asks, down to 0 while a frozen push fades).
    [[nodiscard]] static FfbEffect tuned(const FfbEffect& effect, float pushFade);

    // The target's centring spring (CentringSpring.hpp), 0 to 100 percent; 0 = none. Played on the wheel in a slot
    // of its own (IFfbSink::kCentringSpringSlot) through a limiter of its own with the limits in force, whatever the
    // game sends: its watchdog and "stop all" leave it playing, as Logitech's driver did. Allocation-free; takes
    // effect at the next update(). A wheel that refuses it is not asked again until this is called again.
    void setCentringSpring(float percent);
    [[nodiscard]] float centringSpring() const { return springPercent_; }
    [[nodiscard]] bool centringSpringPlaying() const { return springPlaying_; }

    [[nodiscard]] const Stats& stats() const { return stats_; }

    // How long a frozen push takes to fade out, and the smallest push (of full scale) that counts as one.
    static constexpr std::uint64_t kFrozenFadeNs = 500'000'000;
    static constexpr float kFrozenPushAbove = 0.05F;
    // What the game's pushing forces are multiplied by now: 1, down to 0 while a frozen push fades (last update()).
    [[nodiscard]] float frozenFade() const { return frozenFade_; }

    // A lone full push: a constant level this close to full scale (either way)...
    static constexpr float kFullScaleAt = 0.9995F;
    // ...while the game's last constant force between none and full (a real one) is older than this, or never came.
    static constexpr std::uint64_t kRealForceWithinNs = 2'000'000'000;
    // Whether the game's full pushes play as none now (since its last real force is too old).
    [[nodiscard]] bool ignoringLoneFullPush() const { return ignoringLoneFullPush_; }
    // `effect` as the controller keeps it: a lone full push (`realForceRecent` false) becomes no push; anything else is
    // left as it is.
    [[nodiscard]] static FfbEffect screened(const FfbEffect& effect, bool realForceRecent);

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
        std::optional<FfbEffect> lastLimited;  // the limiter's last output, before the cap on all slots together
        float sentScale = 1.0F;              // the cap's scale in lastSent
    };

    // One sender (onPacket's `source`): when it was last heard, and which of the controller's slots hold its slots.
    struct Source {
        bool used = false;
        std::uint32_t id = 0;
        std::uint64_t lastNs = 0;
        std::array<std::int8_t, IFfbSink::kMaxSlots> slots{};   // its slot n -> the controller's slot, -1: none
    };
    // Its place (made when new), or null when it cannot be had.
    [[nodiscard]] Source* sourceFor(std::uint32_t id, std::uint64_t nowNs);
    // The controller's slot for `source`'s slot `slot`: its own n if free, else any free one; -1 when all are taken.
    [[nodiscard]] int slotFor(Source& source, std::uint8_t slot);
    // `source`'s slots stopped and given up (its "stop all", or its watchdog); the source forgotten when `forget`.
    void release(Source& source, bool forget);

    // Plays, updates or stops the centring spring (update()).
    void updateCentringSpring();

    // The game's constant force as it will play: a lone full push screened out (onPacket), with the log and stats.
    [[nodiscard]] FfbEffect screenFullPush(const FfbEffect& effect, std::uint64_t nowNs);

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
    std::uint64_t frozenNs_ = 0;            // from safety_.frozenForceSeconds; 0 = never
    float frozenFade_ = 1.0F;
    std::optional<std::uint64_t> lastRealForceNs_;   // the game's last constant force between none and full
    bool ignoringLoneFullPush_ = false;
    float springPercent_ = 0.0F;
    bool springPlaying_ = false;
    bool springRefused_ = false;
    std::optional<FfbEffect> springSent_;
    FfbSafetyLimiter springLimiter_;
    bool watchdogTripped_ = false;
    std::array<Source, kMaxSources> sources_{};
    Stats stats_;
    FfbUsage usage_;
    std::optional<std::uint64_t> lastUsageNs_;
};

}  // namespace wheelbri
