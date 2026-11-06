// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/Devices.hpp"
#include "wheelbri/Ffb.hpp"
#include "wheelbri/FfbPacket.hpp"
#include "wheelbri/FfbShape.hpp"
#include "wheelbri/UdpLoopback.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace wheelbri::shim {

// Shim side: the game's DirectInput effects (EmulatedEffect) become FfbPackets for
// wheelbri-service, which limits them and plays them on the physical wheel. One per process.
//
// Timing (duration, iterations, start delay) is tracked here, so the service only ever sees "slot N
// plays effect E" or "slot N is quiet". So are envelopes (attack / fade) and ramps: while one changes the force,
// every tick() sends the level for that moment (FfbShape.hpp). Every change is sent at once; tick() adds a keep-alive and a
// periodic full refresh of every slot (stopped ones too) so a lost datagram heals, a lost "stop" included,
// and the service's watchdog stays fed.
//
// Each slot belongs to the device that created its effect (`owner`, the EmulatedDevice): stopping or resetting
// one device (a game may open the wheel twice) leaves the other's effects alone. An allocation carries a ticket:
// once the slot is freed (Unload, a reset) and handed to another effect, calls with the old ticket do nothing.
class FfbForwarder {
public:
    struct Allocation {
        std::uint8_t slot = 0;
        std::uint32_t ticket = 0;
    };

    [[nodiscard]] static FfbForwarder& instance();

    // Slot for a new effect of `owner`, or nullopt when all are in use (DIERR_DEVICEFULL).
    [[nodiscard]] std::optional<Allocation> allocate(const void* owner);
    // Whether `a` still holds its slot (not freed by Unload or a reset since).
    [[nodiscard]] bool holds(const Allocation& a);
    void release(const Allocation& a);

    // `shape`: the effect's envelope and a ramp's end level, applied as it plays.
    void setEffect(const Allocation& a, const FfbEffect& effect, const FfbShape& shape = {});
    // durationUs / iterations: nullopt = infinite. `solo`: the owner's other effects stop.
    void start(const Allocation& a, std::optional<std::uint64_t> durationUs, std::optional<std::uint32_t> iterations,
               std::uint64_t startDelayUs, bool solo);
    void stop(const Allocation& a);
    [[nodiscard]] bool isPlaying(const Allocation& a);

    // Device-wide commands (DISFFC_*, DIPROP_FFGAIN, unacquire), for `owner`'s effects.
    void stopAll(const void* owner);
    void reset(const void* owner);         // DISFFC_RESET: stop, and free the owner's slots (effects unloaded)
    void setPaused(bool paused);
    void setActuators(bool on);
    void setDeviceGain(float gain);        // 0..1
    [[nodiscard]] float deviceGain();
    [[nodiscard]] std::uint32_t stateFlags();   // DIGFFS_* for GetForceFeedbackState

    // Call often (every input read). Handles finished effects and keep-alives.
    void tick();

private:
    using Clock = std::chrono::steady_clock;

    struct Slot {
        bool inUse = false;
        const void* owner = nullptr;
        std::uint32_t ticket = 0;
        FfbEffect effect;
        FfbShape shape;
        bool started = false;
        Clock::time_point startedAt{};
        std::uint64_t startDelayUs = 0;
        std::optional<std::uint64_t> durationUs;   // one play (iteration); nullopt = infinite
        std::optional<std::uint64_t> lengthUs;   // duration * iterations; nullopt = until stopped
        bool sentPlaying = false;
        std::optional<FfbEffect> sentEffect;
    };

    FfbForwarder() = default;

    [[nodiscard]] bool effectivelyPlaying(const Slot& slot, Clock::time_point now) const;
    [[nodiscard]] bool holdsLocked(const Allocation& a) const;                // caller holds mutex_
    void sendSlot(std::uint8_t index, Clock::time_point now, bool force);   // caller holds mutex_
    void send(const FfbPacket& packet, Clock::time_point now);               // caller holds mutex_
    void ensureSender();                                                      // caller holds mutex_

    std::mutex mutex_;
    std::unique_ptr<UdpLoopbackSender> sender_;
    bool senderTried_ = false;
    std::array<Slot, IFfbSink::kMaxSlots> slots_{};   // same slots as the service
    std::uint32_t sequence_ = 0;
    std::uint32_t nextTicket_ = 0;
    bool paused_ = false;
    bool actuatorsOn_ = true;
    float deviceGain_ = 1.0F;
    Clock::time_point lastSend_{};
    Clock::time_point lastRefresh_{};
};

}  // namespace wheelbri::shim
