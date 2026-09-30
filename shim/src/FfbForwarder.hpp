#pragma once

#include "wheelemu/Devices.hpp"
#include "wheelemu/Ffb.hpp"
#include "wheelemu/FfbPacket.hpp"
#include "wheelemu/UdpLoopback.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace wheelemu::shim {

// Phase 3, shim side: the game's DirectInput effects (EmulatedEffect) become FfbPackets for
// wheelemu-service, which limits them and plays them on the physical wheel. One per process.
//
// Timing (duration, iterations, start delay) is tracked here, so the service only ever sees "slot N
// plays effect E" or "slot N is quiet". Every change is sent at once; tick() adds a keep-alive and a
// periodic full refresh so a lost datagram heals and the service's watchdog stays fed.
class FfbForwarder {
public:
    [[nodiscard]] static FfbForwarder& instance();

    // Slot for a new effect, or nullopt when all are in use (DIERR_DEVICEFULL).
    [[nodiscard]] std::optional<std::uint8_t> allocate();
    void release(std::uint8_t slot);

    void setEffect(std::uint8_t slot, const FfbEffect& effect);
    // durationUs / iterations: nullopt = infinite.
    void start(std::uint8_t slot, std::optional<std::uint64_t> durationUs, std::optional<std::uint32_t> iterations,
               std::uint64_t startDelayUs, bool solo);
    void stop(std::uint8_t slot);
    [[nodiscard]] bool isPlaying(std::uint8_t slot);

    // Device-wide commands (DISFFC_*, DIPROP_FFGAIN, unacquire).
    void stopAll();
    void reset();                          // DISFFC_RESET: stop, and forget what was started
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
        FfbEffect effect;
        bool started = false;
        Clock::time_point startedAt{};
        std::uint64_t startDelayUs = 0;
        std::optional<std::uint64_t> lengthUs;   // duration * iterations; nullopt = until stopped
        bool sentPlaying = false;
        std::optional<FfbEffect> sentEffect;
    };

    FfbForwarder() = default;

    [[nodiscard]] bool effectivelyPlaying(const Slot& slot, Clock::time_point now) const;
    void sendSlot(std::uint8_t index, Clock::time_point now, bool force);   // caller holds mutex_
    void send(const FfbPacket& packet, Clock::time_point now);               // caller holds mutex_
    void ensureSender();                                                      // caller holds mutex_

    std::mutex mutex_;
    std::unique_ptr<UdpLoopbackSender> sender_;
    bool senderTried_ = false;
    std::array<Slot, IFfbSink::kMaxSlots> slots_{};   // same slots as the service
    std::uint32_t sequence_ = 0;
    bool paused_ = false;
    bool actuatorsOn_ = true;
    float deviceGain_ = 1.0F;
    Clock::time_point lastSend_{};
    Clock::time_point lastRefresh_{};
};

}  // namespace wheelemu::shim
