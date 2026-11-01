#include "wheelbri/TrueForce.hpp"

#include "wheelbri/HidWheelDevice.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wheelbri {

namespace {

constexpr std::uint8_t kDescriptor[] = {
    0x06, 0xFD, 0xFF,        // Usage Page 0xFFFD (vendor)
    0x0A, 0x01, 0xFD,        // Usage 0xFD01
    0xA1, 0x01,              // Collection (Application)
    0x85, 0x01,              //   Report ID 0x01
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x3F,  //   63 bytes
    0x09, 0x01, 0x81, 0x02,  //   Input
    0x09, 0x01, 0x91, 0x02,  //   Output
    0xC0,
};

bool sameIgnoringCase(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string typeName(const int type) {
    if (type == 256) return "odd";
    char text[8];
    std::snprintf(text, sizeof(text), "0x%02x", static_cast<unsigned>(type));
    return text;
}

// Up to the last non-zero byte (at least the header).
std::string hex(const std::span<const std::uint8_t> bytes) {
    std::size_t end = bytes.size();
    while (end > 6 && bytes[end - 1] == 0) --end;
    std::string text;
    for (std::size_t i = 0; i < end; ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), i == 0 ? "%02x" : " %02x", static_cast<unsigned>(bytes[i]));
        text += b;
    }
    if (end < bytes.size()) text += " (" + std::to_string(bytes.size() - end) + " zeros)";
    return text;
}

}  // namespace

std::string trueForceHardwareId(const VirtualWheel& wheel) {
    return wheel.trueForce ? std::string(wheel.hardwareId) + std::string(kTrueForceIdSuffix) : std::string();
}

const VirtualWheel* trueForceWheelForHardwareId(const std::string_view hardwareId) {
    for (const VirtualWheel& wheel : virtualWheels()) {
        if (wheel.trueForce && sameIgnoringCase(trueForceHardwareId(wheel), hardwareId)) return &wheel;
    }
    return nullptr;
}

std::span<const std::uint8_t> trueForceReportDescriptor() { return kDescriptor; }

void TrueForceLog::output(const std::span<const std::uint8_t> report, const std::int64_t nowMs) {
    std::lock_guard guard(lock_);
    if (periodStartMs_ < 0) {
        periodStartMs_ = nowMs;
        log_(LogLevel::Info, "trueforce: first packet (something opened the TrueForce interface)");
    }
    const bool packet = report.size() > 5 && report[0] == kTrueForceReportId;
    const int type = packet ? report[4] : kOdd;
    ++period_[type];
    ++total_[type];
    if (type == 0x01 && report.size() > 11) ++streamFlags_[{report[10], report[11]}];
    if (type == 0x01 && report.size() > 11) noteStream(report, nowMs);
    // Every packet but the stream (they are rare), and the stream's first ones and one every kStreamShownMs: what a
    // game sends, raw.
    int& shown = shown_[static_cast<std::size_t>(type)];
    const bool sample = type == 0x01 && (lastStreamShownMs_ < 0 || nowMs - lastStreamShownMs_ >= kStreamShownMs);
    if (type != 0x01 || shown < kFirstShown || sample) {
        ++shown;
        if (type == 0x01) lastStreamShownMs_ = nowMs;
        log_(LogLevel::Debug, "trueforce: " + typeName(type) + " #" + std::to_string(shown) +
                                  " (" + std::to_string(report.size()) + " bytes): " + hex(report));
    }
    if (nowMs - periodStartMs_ >= kSummaryMs) summarise(nowMs);
}

void TrueForceLog::finish(const std::int64_t nowMs) {
    std::lock_guard guard(lock_);
    if (total_.empty()) {
        log_(LogLevel::Info, "trueforce: nothing was sent to the TrueForce interface");
        return;
    }
    if (!period_.empty()) summarise(nowMs);
    std::string line = "trueforce: in all:";
    for (const auto& [type, count] : total_) line += " " + typeName(type) + " x" + std::to_string(count);
    log_(LogLevel::Info, line);
}

namespace {

// An offset-binary u16 (0x8000 = 0) as -1..1.
double level(const std::span<const std::uint8_t> r, const std::size_t at) {
    const int raw = r[at] | (r[at + 1] << 8);
    return (raw - 0x8000) / 32768.0;
}

}  // namespace

void TrueForceLog::noteStream(const std::span<const std::uint8_t> r, const std::int64_t nowMs) {
    constexpr double kFlipAt = 0.02;
    const int raw = streamTarget(r);
    // The SDK clamps cur to 1..0xFFFF: a computed NaN or overflow lands on 0x0001 and stays (2026-10-28).
    const bool atLimit = raw <= 0x0001 || raw >= 0xFFFF;
    if (!atLimit) {
        pinnedSinceMs_ = -1;
    } else if (pinnedSinceMs_ < 0) {
        pinnedSinceMs_ = nowMs;
        pinnedShown_ = false;
        // How it got there: the last kHistory values of cur (hex), oldest first, ms before this packet.
        std::string line = "trueforce: the stream's force hit its limit; before it (ms ago: cur):";
        for (std::size_t i = 0; i < kHistory; ++i) {
            const auto& [ms, value] = history_.at((historyNext_ + i) % kHistory);
            if (ms < 0) continue;
            char item[24];
            std::snprintf(item, sizeof(item), " %lld:%04x", static_cast<long long>(nowMs - ms), value);
            line += item;
        }
        log_(LogLevel::Warn, line);
    } else if (!pinnedShown_ && nowMs - pinnedSinceMs_ >= kPinnedMs) {
        pinnedShown_ = true;
        log_(LogLevel::Warn, "trueforce: the stream's force sits at its limit for " + std::to_string(kPinnedMs) +
                                 " ms: " + hex(r));
    }
    history_.at(historyNext_) = {nowMs, raw};
    historyNext_ = (historyNext_ + 1) % kHistory;
    const double cur = (raw - 0x8000) / 32768.0;
    stream_.curMin = std::min(stream_.curMin, cur);
    stream_.curMax = std::max(stream_.curMax, cur);
    stream_.curAbsSum += std::abs(cur);
    ++stream_.curCount;
    const int side = cur > kFlipAt ? 1 : cur < -kFlipAt ? -1 : 0;
    if (side != 0) {
        if (stream_.side != 0 && side != stream_.side) ++stream_.flips;
        stream_.side = side;
    }
    // The window: 13 slots of 4 bytes (left, right) from byte 12, newest last; byte 10 new ones, byte 11 0x0d = valid.
    constexpr std::size_t kSlots = 13;
    if (r[11] != 0x0D || r.size() < 12 + kSlots * 4) return;
    const std::size_t fresh = std::min<std::size_t>(r[10], kSlots);
    for (std::size_t slot = kSlots - fresh; slot < kSlots; ++slot) {
        stream_.texturePeak = std::max(stream_.texturePeak, std::abs(level(r, 12 + slot * 4)));
        ++stream_.samples;
    }
}

void TrueForceLog::summarise(const std::int64_t nowMs) {
    const std::int64_t spanMs = std::max<std::int64_t>(nowMs - periodStartMs_, 1);
    std::string line = "trueforce: last " + std::to_string(spanMs / 1000) + " s:";
    for (const auto& [type, count] : period_) {
        line += " " + typeName(type) + " x" + std::to_string(count);
        if (type != 0x01) continue;
        line += " (" + std::to_string(count * 1000 / static_cast<std::uint64_t>(spanMs)) + "/s; bytes 10/11";
        for (const auto& [flags, n] : streamFlags_) {
            char b[16];
            std::snprintf(b, sizeof(b), " %02x/%02x", static_cast<unsigned>(flags.first),
                          static_cast<unsigned>(flags.second));
            line += b + std::string(" x") + std::to_string(n);
        }
        if (stream_.curCount > 0) {
            char b[160];
            std::snprintf(b, sizeof(b), "; cur %.2f..%.2f avg|.| %.2f flips %llu; texture peak %.2f in %llu samples",
                          stream_.curMin, stream_.curMax, stream_.curAbsSum / static_cast<double>(stream_.curCount),
                          static_cast<unsigned long long>(stream_.flips), stream_.texturePeak,
                          static_cast<unsigned long long>(stream_.samples));
            line += b;
        }
        line += ")";
    }
    log_(LogLevel::Debug, line);
    period_.clear();
    streamFlags_.clear();
    stream_ = Stream{};
    periodStartMs_ = nowMs;
}

}  // namespace wheelbri

namespace wheelbri {

namespace {

// How Logitech's DLL reads a status (its parser, trueforce_sdk_x64.dll 1.3.12): angle = -(position - range / 2) /
// (p4 * p5) * 2 pi, speed = -speed / (p4 * p5) * 2 pi, full-scale torque = (t1 + t2) / 65536000 * 0.5 (Nm): the
// stream's force ("cur", 6..9) = centre - torque / full scale * centre, none at all while the full scale is 0.
// The values are a real G923's (status type 0x02, 2026-11-01).
constexpr std::uint32_t kCountRange = 0x0A476B;   // [22..24]: 673643 counts = 904.5 degrees, a 900-degree wheel
// p4 * p5 = 16384 * 16.3636 as float32 (the DLL's own product): 268101.8 counts a turn, 744.7 a degree.
constexpr double kCountsPerTurn =
    static_cast<double>(std::bit_cast<float>(0x46800000U) * std::bit_cast<float>(0x4182E8BAU));
constexpr std::int64_t kCentre = kCountRange / 2;
// [25..28], [29..32]: the full-scale torque, 0.65 Nm = 0x80000000 + 0.65 * 65536000 each (the SDK's peak torque,
// 2 * p8 * p10 * p11 = 2.53 Nm, comes from the parameters, not from here).
constexpr std::uint32_t kFullScaleTorque = 0x828A0000U;

void put24(TrueForceDevice::Report& r, const std::size_t at, const std::uint32_t v) {
    for (std::size_t i = 0; i < 3; ++i) r[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

void putU16(TrueForceDevice::Report& r, const std::size_t at, const std::uint16_t v) {
    r[at] = static_cast<std::uint8_t>(v & 0xFF);
    r[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

void putU32(TrueForceDevice::Report& r, const std::size_t at, const std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) r[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Every answer ends in a checksum: the bytes from the type up to it sum to 0 (mod 256). Found from the captures; a host
// that gets an answer without it sends its question again (every 200 ms, about 10 times).
void seal(TrueForceDevice::Report& r, const std::size_t at) {
    unsigned sum = 0;
    for (std::size_t i = 4; i < at; ++i) sum += r[i];
    r[at] = static_cast<std::uint8_t>(0x100U - (sum & 0xFFU));
}

TrueForceDevice::Report blank(const std::uint8_t type, const std::uint8_t seq) {
    TrueForceDevice::Report r(kTrueForceReportSize, 0);
    r[0] = kTrueForceReportId;
    r[4] = type;
    r[5] = seq;
    return r;
}

std::string degrees(const float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(value));
    return text;
}

}  // namespace

TrueForceDevice::TrueForceDevice(TrueForceLog::Logger log) : say_(log), log_(std::move(log)) {}

namespace {

// The 64 parameters 0x00..0x3F as a real G923 (046D:C26E, firmware U1 39.02) answers them, bit for bit (float32):
// measured 2026-11-01 from ACC's set-up (S2) and a read-only probe of all 64 (S6), the same with G HUB open or closed
// (Claude outputs/plans/g923-capture-results.md). 0x2F..0x3F: ff ff ff ff (NaN), the wheel has none of them.
constexpr std::array<std::uint32_t, 64> kG923ParameterBits = {
    0x40000000U, 0x40000000U, 0x47000000U, 0x477FFF00U, 0x46800000U, 0x4182E8BAU, 0x3F19999AU, 0x3ECCCCCDU,   // 0x00..
    0x3F267B5FU, 0x3F12AD5DU, 0x4182E8BAU, 0x3DF34D6AU, 0x4175BE0BU, 0x3F32B8C2U, 0xC0F5BE0BU, 0x40F5BE0BU,   // 0x08..
    0x41500000U, 0x40000000U, 0x457A0000U, 0x41200000U, 0x44FA0000U, 0x43E10000U, 0x00000000U, 0x41F00000U,   // 0x10..
    0x42200000U, 0x3F800000U, 0x3F000000U, 0x3F800000U, 0x3E99999AU, 0x40800000U, 0x40000000U, 0x477FFD00U,   // 0x18..
    0x477D0100U, 0x3F800000U, 0x477F0000U, 0x40000000U, 0x00000000U, 0x3F800000U, 0x40800000U, 0x00000000U,   // 0x20..
    0x00000000U, 0x3F800000U, 0x40400000U, 0x41800000U, 0x3C75C28FU, 0x3DCCCCCDU, 0x40400000U, 0xFFFFFFFFU,   // 0x28..
    0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,   // 0x30..
    0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,   // 0x38..
};

// 0x34..0x38 switch the SDK's slots 1..5 on when negative (its set-up compares 0 > the parameter); a G923's are NaN, so
// all stay off. WheelBri answered 0x36 = -1 until 2026-11-01 (slot 3 on: no KF reconstruction filter, which ran away
// with the old direct-drive parameters, 2026-10-28). With it, ACC's stream leaned one way (a steady pull to the left on
// the R3, 2026-11-01), so the real value is tried (TrueForce plan, step 4).

}  // namespace

std::uint32_t TrueForceDevice::parameterBits(const std::uint8_t index) {
    return index < kG923ParameterBits.size() ? kG923ParameterBits[index] : 0U;   // beyond 0x3F: never asked
}

float TrueForceDevice::parameter(const std::uint8_t index) { return std::bit_cast<float>(parameterBits(index)); }

bool TrueForceDevice::running() const {
    std::lock_guard guard(lock_);
    return running_;
}

TrueForceDevice::Report TrueForceDevice::status(const std::uint8_t type, const std::uint8_t seq,
                                                const std::int64_t nowUs, const std::optional<std::uint32_t> at17) {
    Report r = blank(type, seq);
    // The steering over the wheel's rotation: its end stop = half of it each way, as a real wheel's position is its
    // angle whatever the game's range. The DLL's angle is + to the left (ACC turned the other way with + to the right,
    // 2026-10-27), so a right turn counts up.
    const std::int64_t counts = positionCounts();
    put24(r, 6, static_cast<std::uint32_t>(std::clamp<std::int64_t>(counts, 0, kCountRange)));
    putU16(r, 9, 0x8000);
    putU16(r, 11, 0x8000);
    putU32(r, 13, static_cast<std::uint32_t>(nowUs));
    const auto wireSpeed =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(std::clamp(speed_, -2e9, 2e9)) + 0x80000000LL);
    putU32(r, 17, at17.value_or(wireSpeed));
    put24(r, 22, kCountRange);
    putU32(r, 25, kFullScaleTorque);
    putU32(r, 29, kFullScaleTorque);
    seal(r, 33);
    lastStatusUs_ = nowUs;
    return r;
}

std::int64_t TrueForceDevice::positionCounts() const {
    const double degreesRight = (static_cast<double>(position_) - 32768.0) / 32768.0 * rotationDeg_ / 2.0;
    return static_cast<std::int64_t>(std::llround(kCentre + degreesRight / 360.0 * kCountsPerTurn));
}

std::vector<TrueForceDevice::Report> TrueForceDevice::output(const std::span<const std::uint8_t> report,
                                                             const std::int64_t nowUs) {
    log_.output(report, nowUs / 1000);
    std::vector<Report> answers;
    if (report.size() < 6 || report[0] != kTrueForceReportId) return answers;
    const std::uint8_t type = report[4];
    const std::uint8_t seq = report[5];
    const auto at = [&report](const std::size_t i) -> std::uint8_t { return i < report.size() ? report[i] : 0; };
    std::lock_guard guard(lock_);
    lastSeq_ = seq;
    switch (type) {
        case 0x05: {
            const std::uint8_t index = at(6);
            Report r = blank(0x05, seq);
            r[6] = index;
            r[7] = 0x09;
            r[8] = index;
            putU32(r, 9, parameterBits(index));   // the bits as they are: NaN stays ff ff ff ff
            seal(r, 13);
            answers.push_back(std::move(r));
            break;
        }
        case 0x07: {
            Report r = blank(0x07, seq);
            r[6] = 0x06;
            constexpr std::uint8_t kSlotFlags[] = {1, 1, 1, 1, 0, 1};
            for (std::size_t slot = 0; slot < 6; ++slot) {
                r[7 + slot * 3] = static_cast<std::uint8_t>(slot + 1);
                r[8 + slot * 3] = 0x01;
                r[9 + slot * 3] = kSlotFlags[slot];
            }
            seal(r, 25);
            answers.push_back(std::move(r));
            break;
        }
        case 0x06: {
            answers.push_back(status(0x10, seq, nowUs, 0xFFFFFFFFU));
            break;
        }
        case 0x0E: {
            std::uint32_t bits = 0;
            for (std::size_t i = 0; i < 4; ++i) bits |= static_cast<std::uint32_t>(at(6 + i)) << (8 * i);
            float range = 0;
            std::memcpy(&range, &bits, sizeof(range));
            say_(LogLevel::Info, "trueforce: operating range " + degrees(range) + " degrees");
            // A real G923 answers it with a plain status carrying its sequence, as any other packet (ACC, 2026-11-01:
            // frame 261 of S2). Without one the SDK resends it for 2 s, then takes no answer at all: no session, no
            // angle, the car does not steer (2026-11-01, Mode Virtual).
            answers.push_back(status(0x02, seq, nowUs));
            break;
        }
        case 0x03:
        case 0x04:
            if ((type == 0x03) != running_) {
                say_(LogLevel::Info, type == 0x03 ? "trueforce: session started" : "trueforce: session stopped");
            }
            running_ = type == 0x03;
            answers.push_back(status(0x02, seq, nowUs));
            break;
        case 0x01:
            answers.push_back(status(0x02, seq, nowUs));
            break;
        default:
            break;
    }
    return answers;
}

std::optional<TrueForceDevice::Report> TrueForceDevice::steering(const std::int32_t value, const int rotationDeg,
                                                                const std::int64_t nowUs) {
    const auto clamped = static_cast<std::uint16_t>(std::clamp(value, 0, 65535));
    std::lock_guard guard(lock_);
    position_ = clamped;
    if (rotationDeg >= 90 && rotationDeg <= 2700) rotationDeg_ = rotationDeg;
    // The speed between steering samples (the service's state, 2 ms apart), half new, half old: a status sent just
    // after a sample must not divide its step by the microseconds since the last status (spikes of 20 times the real
    // speed, 2026-10-27).
    const std::int64_t counts = positionCounts();
    if (lastSampleUs_ >= 0 && nowUs > lastSampleUs_) {
        const double raw = static_cast<double>(counts - lastSampleCounts_) * 1e6 /
                           static_cast<double>(nowUs - lastSampleUs_);
        speed_ += kSpeedSmoothing * (raw - speed_);
    }
    lastSampleCounts_ = counts;
    lastSampleUs_ = nowUs;
    fastest_ = std::max(fastest_, std::abs(speed_));
    if (speedLogUs_ < 0) speedLogUs_ = nowUs;
    if (nowUs - speedLogUs_ >= TrueForceLog::kSummaryMs * 1000) {
        if (running_) {
            say_(LogLevel::Debug, "trueforce: last 10 s: the wheel turned at most " +
                                      std::to_string(std::lround(fastest_ / kCountsPerTurn * 360.0)) + " deg/s");
        }
        fastest_ = 0;
        speedLogUs_ = nowUs;
    }
    if (!running_ || (lastStatusUs_ >= 0 && nowUs - lastStatusUs_ < kStatusEveryUs)) return std::nullopt;
    return status(0x02, lastSeq_, nowUs);
}

}  // namespace wheelbri
