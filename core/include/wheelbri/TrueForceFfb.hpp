// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/FfbPacket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>

namespace wheelbri {

// The force a TrueForce game streams (Logitech's DLL, TrueForce.hpp), played: the stream's motor torque target
// ("cur", type 0x01: streamTarget(), the mean of the words at 6..7 and 8..9, offset binary around 0x8000) becomes a
// constant force in service slot kSlot, as HID++ and HID PID effects become FfbPackets. 0x0000 = +1, in the sign
// games' DirectInput forces use as WheelBri passes them on: ACC's torque + pushes left (it opposes the angle,
// + = left), Logitech's DLL streams it below 0x8000, and on a Moza R3 that plays a -1 to the right, as games' forces
// do (--ffb-test, 2026-10-28).
//   - a changed level goes out at once, but at most one every kMinGapUs (the stream runs at about 640 a second);
//     the last one held back goes out from tick()
//   - while it plays, the level again every kKeepAliveUs (a lost datagram heals; the service's watchdog stays fed)
//   - a stop (0x04) sets it to 0 but keeps it playing (the SDK stops and starts again when a game re-opens the wheel)
//   - quiet (playing = false) when no stream came for kQuietUs (a game paused or gone), and on stop()
// The damping the game sets (0x0c, f32 at 6..9: logiTrueForceSetDamping, 0..kDampingMax), which a G923 applies itself,
// plays alongside as a damper in kDamperSlot, coefficient = damping / kDampingMax: sent when it changes, with the
// force's keep-alives, and quiet with the force (or at a damping of 0). A direct-drive wheel swings without it.
// The texture (the stream's sample window: byte 10 the new samples, byte 11 0x0d while valid, 13 slots of two
// offset-binary words from byte 12, newest last; texture() decodes a sample as the mean of its two words, which lean
// opposite ways as the torque's do) plays as a sine in kTextureSlot. A real G923 adds the samples to its motor torque,
// 4 a millisecond; the rig takes an effect at most every 4 ms (FfbController's minUpdateNs), so the texture is summed
// up over kTextureSamples (40 ms) and the rig makes the vibration itself:
//   - magnitude: sqrt(2) x the window's RMS about its mean, at most kTextureMax; below kTextureQuiet, none
//   - period: from the zero crossings (past kTextureHysteresis) of the running mean of kTextureSmoothing samples
//     (of the samples themselves when it has fewer than two), twice the mean gap between them; none: the slowest;
//     within kTextureMinPeriodMs..kTextureMaxPeriodMs
//   - sent when it changes enough to feel (kTextureStep, kTexturePeriodStep: a periodic effect sent anew may restart
//     its phase), with the force's keep-alives, and quiet with the force or at a stop (0x04)
// Measured in ACC on a real G923 (2026-11-01, S2's stream.tsv): RMS 0.015, peak 0.12, most of it 100..500 Hz; per
// 40 ms window a magnitude of 0.02 (median; 0.04 at the 99th percentile) and its strongest frequency 240 Hz (median),
// which these crossings find within 20 Hz.
// Thread-safe: writes and ticks come on different threads.
class TrueForceFfb {
public:
    using Sink = std::function<void(const FfbPacket&)>;
    static constexpr std::uint8_t kSlot = 7;              // the service's last slot (IFfbSink::kMaxSlots - 1)
    static constexpr std::uint8_t kDamperSlot = 6;
    static constexpr float kDampingMax = 0.3F;            // logiTrueForceGetDampingMax (TrueForce.cpp's parameter 9)
    static constexpr std::int64_t kMinGapUs = 2'000;
    static constexpr std::int64_t kKeepAliveUs = 100'000;
    static constexpr std::int64_t kQuietUs = 200'000;
    static constexpr std::uint8_t kTextureSlot = 5;
    static constexpr std::size_t kTextureSamples = 160;      // 40 ms at 4 samples a millisecond
    static constexpr float kTextureSampleRateHz = 4000.0F;   // 4 new samples a packet, 1000 packets a second
    static constexpr float kTextureMax = 0.25F;              // twice the peak a real G923 got from ACC
    static constexpr float kTextureQuiet = 0.003F;           // about 100 counts: below it, no texture
    static constexpr float kTextureHysteresis = 0.001F;      // a zero crossing counts past this either side
    static constexpr std::size_t kTextureSmoothing = 4;      // samples in the running mean the crossings are of
    static constexpr float kTextureMinPeriodMs = 2.0F;       // 500 Hz
    static constexpr float kTextureMaxPeriodMs = 100.0F;     // 10 Hz
    static constexpr float kTextureStep = 0.2F;              // a new magnitude this much (relative) off is sent
    static constexpr float kTexturePeriodStep = 0.15F;       // as is a new period

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
    // A stream packet's new texture samples, oldest first, each -1..1 in the force's sign (0x0000 = 1): the newest
    // byte-10 of the window's 13 slots; none while the window is not valid (byte 11 not 0x0d) or for anything else.
    // `out` gets at most out.size(); the number written is returned.
    [[nodiscard]] static std::size_t texture(std::span<const std::uint8_t> report, std::span<float> out);

private:
    void send(bool playing, std::int64_t nowUs);
    void updateDamper(std::int64_t nowUs);
    void sendDamper(bool playing, std::int64_t nowUs);
    // The new samples into the window; a full window becomes the next texture (sent when it changed enough).
    void addTexture(std::span<const float> samples, std::int64_t nowUs);
    void updateTexture(std::int64_t nowUs);
    void startWindow();    // the next window from nothing
    void resetTexture();   // no texture, and nothing of the last samples kept (a stop, or quiet)
    void sendTexture(bool playing, std::int64_t nowUs);

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
    // The texture's window so far.
    std::size_t windowCount_ = 0;
    double windowSum_ = 0;
    double windowSumSq_ = 0;
    struct Crossings {
        int side = 0;                  // -1, 0, 1: the last side past kTextureHysteresis
        std::size_t count = 0;         // this window
        std::size_t first = 0;         // the first's and the last's sample in the window
        std::size_t last = 0;
    };
    Crossings smoothCrossings_;        // of the running mean
    Crossings rawCrossings_;           // of the samples themselves
    std::array<float, kTextureSmoothing> smooth_{};   // the last samples, for the running mean
    std::size_t smoothNext_ = 0;
    float smoothSum_ = 0.0F;
    float textureMagnitude_ = 0.0F;    // the newest, 0..kTextureMax (0: none)
    float texturePeriodMs_ = 0.0F;
    float sentTextureMagnitude_ = 0.0F;
    float sentTexturePeriodMs_ = 0.0F;
    bool texturePlaying_ = false;
    std::int64_t lastTextureUs_ = -1;
};

}  // namespace wheelbri
