#pragma once

#include "wheelbri/LogLevel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wheelbri {

struct VirtualWheel;

// Logitech TrueForce, device side: a real G923 has it on its own USB interface (interface 1; interface 0 carries the
// game controller and HID++), a vendor collection (usage page 0xFFFD, usage 0xFD01) with report 0x01, 63 bytes each
// way. Logitech's TrueForce SDK
// (trueforce_sdk_x64.dll, from G HUB) finds it with SetupDi + HidD_GetAttributes + HidP_GetCaps and pairs it with the
// wheel by container id, then streams packets to it ([01 00 00 00, type, sequence, payload], type 0x01 the ~1 kHz
// torque + texture stream). Written from public protocol descriptions; nothing is copied from GPL code.
//
// The virtual wheel's input report already is 0x01, so the interface is a second software device: the wheel's
// hardware id + kTrueForceIdSuffix (wheelbri\G923_TF), in the wheel's container ([emulator] trueforce). It
// answers as a real wheel (TrueForceDevice) and logs what it is sent (TrueForceLog); it plays nothing yet.
inline constexpr std::string_view kTrueForceIdSuffix = "_TF";
inline constexpr std::uint8_t kTrueForceReportId = 0x01;
inline constexpr std::size_t kTrueForceReportSize = 64;   // with the report ID

// "wheelbri\G923_TF" for a wheel with TrueForce (VirtualWheel::trueForce); empty for one without.
[[nodiscard]] std::string trueForceHardwareId(const VirtualWheel& wheel);
// The wheel whose TrueForce interface `hardwareId` is (any case); null when it is none.
[[nodiscard]] const VirtualWheel* trueForceWheelForHardwareId(std::string_view hardwareId);
[[nodiscard]] std::span<const std::uint8_t> trueForceReportDescriptor();

// The service sends its state here too (the same StatePacket as to the wheel) while trueforce is on: the TrueForce
// device reports the steering from it. Its own port, as the wheel's (41329) takes one reader only.
inline constexpr std::uint16_t kTrueForceStatePort = 41331;

// What a host sends the TrueForce interface, for the log: every packet in hex but the stream's (its first kFirstShown,
// then one every kStreamShownMs), and every kSummaryMs a line of counts per type (for the stream, also its bytes 10/11:
// new samples, window flag).
// For the stream also its force: cur (bytes 6..7, offset binary, 0x8000 = none) as -1..1, its range, mean size and
// how often it changed side, and the texture samples' peak.
// Thread-safe: the driver's writes come on a parallel queue.
class TrueForceLog {
public:
    using Logger = std::function<void(LogLevel, const std::string&)>;
    static constexpr int kFirstShown = 3;
    static constexpr std::int64_t kStreamShownMs = 250;   // a stream packet logged raw this often
    static constexpr std::int64_t kSummaryMs = 10'000;
    static constexpr std::int64_t kPinnedMs = 1'000;   // the stream's force at full this long: logged once, raw

    explicit TrueForceLog(Logger log) : log_(std::move(log)) {}
    TrueForceLog(const TrueForceLog&) = delete;
    TrueForceLog& operator=(const TrueForceLog&) = delete;

    // `report`: as written, report ID first.
    void output(std::span<const std::uint8_t> report, std::int64_t nowMs);
    // The counts since the last summary, and the totals (the device goes away).
    void finish(std::int64_t nowMs);

private:
    // Not a TrueForce packet: a wrong report ID or too short to have a type.
    static constexpr int kOdd = 256;

    void summarise(std::int64_t nowMs);
    void noteStream(std::span<const std::uint8_t> report, std::int64_t nowMs);

    Logger log_;
    std::mutex lock_;
    std::array<int, 257> shown_{};
    std::map<int, std::uint64_t> period_;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint64_t> streamFlags_;
    // The stream's force (cur, bytes 6..7) and texture (the new samples at the window's end) this period, as -1..1.
    struct Stream {
        double curMin = 1;
        double curMax = -1;
        double curAbsSum = 0;
        std::uint64_t curCount = 0;
        std::uint64_t flips = 0;   // cur crossing from one side to the other (beyond kFlipAt)
        int side = 0;
        double texturePeak = 0;
        std::uint64_t samples = 0;
    } stream_;
    std::map<int, std::uint64_t> total_;
    std::int64_t periodStartMs_ = -1;
    std::int64_t pinnedSinceMs_ = -1;
    std::int64_t lastStreamShownMs_ = -1;
    bool pinnedShown_ = false;
    static constexpr std::size_t kHistory = 40;   // the stream's last cur values, logged when it hits its limit
    std::array<std::pair<std::int64_t, int>, kHistory> history_ = [] {
        std::array<std::pair<std::int64_t, int>, kHistory> a{};
        a.fill({-1, 0});
        return a;
    }();
    std::size_t historyNext_ = 0;
};

}  // namespace wheelbri

namespace wheelbri {

// The TrueForce interface's answers, as a real G923 gives them (captured 2026-11-01 with ACC, firmware U1 39.02;
// 07 and 06 from a direct-drive wheel: the SDK sends neither to a G923). Bytes after the [01 00 00 00] header, s =
// the host's sequence:
//   05 s idx        -> 05 s idx 09 idx <f32 value>        a parameter's current value (parameterBits())
//   07 s            -> 07 s 06, then six slots <n 01 flag>
//   06 s ..         -> status of type 10, ff ff ff ff at 17..20
//   0e s <f32 deg>  -> status 02 (operating range: logged)
//   03 s / 04 s     -> status 02 (03 starts the session, 04 stops it)
//   01 s .. (stream)-> status 02 (the stream comes 1000 times a second)
//   09, 0b, 0c, ... -> nothing
// A status report, as Logitech's DLL (1.3.12) parses it: [4] type, [5] s, [6..8] the wheel's position in counts
// (24-bit; centre = half of [22..24]), [9..12] 00 80 00 80, [13..16] a clock in microseconds, [17..20] the speed in
// counts a second (int32 + 0x80000000), [22..24] the count range (673643 = 904.5 degrees at 268101.8 a turn: p4 * p5),
// [25..32] the full-scale torque, two int32 + 0x80000000 (0.65 Nm), [33] the checksum. Every answer ends in one
// (seal: its bytes from [4] sum to 0). The DLL's angle = -(position - centre) / (p4 * p5) turns, + = left: a right turn
// counts up.
// While a session runs, a new steering value also sends a status 02 (steering()), at most one a millisecond, so the
// game reads the wheel even when it streams nothing. Thread-safe: writes and state come on different threads.
class TrueForceDevice {
public:
    using Report = std::vector<std::uint8_t>;   // an input report for the host: kTrueForceReportSize, report ID first
    static constexpr std::int64_t kStatusEveryUs = 1000;
    static constexpr double kSpeedSmoothing = 0.5;   // the new sample's share of the speed

    explicit TrueForceDevice(TrueForceLog::Logger log);

    // What the host wrote (report ID first): its answers, in order (none for most stream-less packets).
    [[nodiscard]] std::vector<Report> output(std::span<const std::uint8_t> report, std::int64_t nowUs);
    // The steering from the service's state (0..65535, clamped) over the wheel's rotation (90..2700 degrees; anything
    // else keeps the last one): a status report when a session runs and the last one went out kStatusEveryUs ago.
    // The count range spans 904.5 degrees, as a G923's: a rotation above that reaches the ends early.
    [[nodiscard]] std::optional<Report> steering(std::int32_t value, int rotationDeg, std::int64_t nowUs);
    void finish(std::int64_t nowUs) { log_.finish(nowUs / 1000); }
    [[nodiscard]] bool running() const;
    // A parameter's value as a real G923 reports it, as float32 bits (0x2F..0x3F: NaN, ff ff ff ff); 0 beyond 0x3F. parameter(): the same as a float.
    [[nodiscard]] static std::uint32_t parameterBits(std::uint8_t index);
    [[nodiscard]] static float parameter(std::uint8_t index);

private:
    // `at17`: bytes 17..20; none = the wheel's speed (a plain status).
    [[nodiscard]] std::int64_t positionCounts() const;   // position_ over rotationDeg_, as the status carries it
    Report status(std::uint8_t type, std::uint8_t seq, std::int64_t nowUs,
                  std::optional<std::uint32_t> at17 = std::nullopt);

    TrueForceLog::Logger say_;
    TrueForceLog log_;
    mutable std::mutex lock_;
    bool running_ = false;
    std::uint16_t position_ = 0x8000;
    int rotationDeg_ = 900;            // the wheel's rotation (target_rotation_deg): its end stop is half of it
    double speed_ = 0;                 // counts a second, from the steering samples (steering())
    std::int64_t lastSampleCounts_ = 0;
    std::int64_t lastSampleUs_ = -1;
    double fastest_ = 0;               // the largest |speed_| since speedLogUs_ (logged every 10 s in a session)
    std::int64_t speedLogUs_ = -1;
    std::uint8_t lastSeq_ = 0;
    std::int64_t lastStatusUs_ = -1;
};

}  // namespace wheelbri
