#pragma once

#include "wheelbri/FfbPacket.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>

namespace wheelbri {

// The force a TrueForce game streams (Logitech's DLL, TrueForce.hpp), played: the stream's motor torque target
// ("cur", type 0x01 bytes 6..7, offset binary around 0x8000; 8..9 repeat it) becomes a constant force in service slot
// kSlot, as HID++ and HID PID effects become FfbPackets. 0x0000 = +1, in the sign games' DirectInput forces use as
// WheelBri passes them on: ACC's torque + pushes left (it opposes the angle, + = left), Logitech's DLL streams it
// below 0x8000, and on a Moza R3 that plays a -1 to the right, as games' forces do (--ffb-test, 2026-10-28).
//   - a changed level goes out at once, but at most one every kMinGapUs (the stream runs at about 640 a second);
//     the last one held back goes out from tick()
//   - while it plays, the level again every kKeepAliveUs (a lost datagram heals; the service's watchdog stays fed)
//   - a stop (0x04) sets it to 0 but keeps it playing (the SDK stops and starts again when a game re-opens the wheel)
//   - quiet (playing = false) when no stream came for kQuietUs (a game paused or gone), and on stop()
// The damping the game sets (0x0c, f32 at 6..9: logiTrueForceSetDamping, 0..kDampingMax), which a G923 applies itself,
// plays alongside as a damper in kDamperSlot, coefficient = damping / kDampingMax: sent when it changes, with the
// force's keep-alives, and quiet with the force (or at a damping of 0). A direct-drive wheel swings without it.
// The texture window (bytes 10..63) is not played. Thread-safe: writes and ticks come on different threads.
class TrueForceFfb {
public:
    using Sink = std::function<void(const FfbPacket&)>;
    static constexpr std::uint8_t kSlot = 7;              // the service's last slot (IFfbSink::kMaxSlots - 1)
    static constexpr std::uint8_t kDamperSlot = 6;
    static constexpr float kDampingMax = 0.3F;            // logiTrueForceGetDampingMax (TrueForce.cpp's parameter 9)
    static constexpr std::int64_t kMinGapUs = 2'000;
    static constexpr std::int64_t kKeepAliveUs = 100'000;
    static constexpr std::int64_t kQuietUs = 200'000;

    explicit TrueForceFfb(Sink sink);

    // A host write to the TrueForce interface, report ID first: a stream packet moves the force, a stop ends it.
    void output(std::span<const std::uint8_t> report, std::int64_t nowUs);
    // About once a millisecond: a held-back level, keep-alives, and quiet after kQuietUs without a stream.
    void tick(std::int64_t nowUs);
    // Quiet now (the interface closed).
    void stop(std::int64_t nowUs);
    [[nodiscard]] bool playing() const;

    // A stream packet's force, -1 .. 1: 0x0000 = 1, 0xFFFF = about -1; nullopt for anything else.
    [[nodiscard]] static std::optional<float> level(std::span<const std::uint8_t> report);

private:
    void send(bool playing, std::int64_t nowUs);
    void updateDamper(std::int64_t nowUs);
    void sendDamper(bool playing, std::int64_t nowUs);

    Sink sink_;
    mutable std::mutex lock_;
    bool playing_ = false;
    float level_ = 0.0F;             // the newest level
    float sentLevel_ = 0.0F;
    bool pending_ = false;           // level_ not sent yet
    std::int64_t lastSentUs_ = -1;
    std::int64_t lastStreamUs_ = -1;
    std::uint32_t sequence_ = 0;
    float damping_ = 0.0F;           // the newest damper coefficient, 0..1
    float sentDamping_ = 0.0F;
    bool damperPlaying_ = false;
    std::int64_t lastDamperUs_ = -1;
};

}  // namespace wheelbri
