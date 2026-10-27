#include "wheelbri/TrueForce.hpp"

#include "wheelbri/HidWheelDevice.hpp"

#include <algorithm>
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
    if (type == 0x01 && report.size() > 11) noteStream(report);
    if (int& shown = shown_[static_cast<std::size_t>(type)]; shown < kFirstShown) {
        ++shown;
        log_(LogLevel::Info, "trueforce: " + typeName(type) + " #" + std::to_string(shown) +
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

void TrueForceLog::noteStream(const std::span<const std::uint8_t> r) {
    constexpr double kFlipAt = 0.02;
    const double cur = level(r, 6);
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
    log_(LogLevel::Info, line);
    period_.clear();
    streamFlags_.clear();
    stream_ = Stream{};
    periodStartMs_ = nowMs;
}

}  // namespace wheelbri

namespace wheelbri {

namespace {

constexpr std::uint8_t kStatusTail[] = {0x00, 0x00, 0x80, 0x07, 0x00, 0x00, 0x1D, 0x95, 0x00, 0x00, 0x1D, 0x95};

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

// The steering as a status carries it: signed, centre 0, + = left (ACC read 0x8000 as full left lock and the plain
// signed value turned the car the other way, 2026-10-27).
std::uint16_t wireSteering(const std::uint16_t steering) {
    const int value = std::clamp(0x8000 - static_cast<int>(steering), -0x8000, 0x7FFF);
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(value));
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

float TrueForceDevice::parameter(const std::uint8_t index) {
    switch (index) {
        case 0x00: return 2.0F;
        case 0x01: return 1.0F;
        case 0x02: return 32768.0F;
        case 0x03: return 65535.0F;
        case 0x07: return 5.4054F;
        case 0x09: return 0.3F;
        case 0x0C: return 47.1239F;
        case 0x0D: return 1.5708F;
        case 0x0E: return -9.4248F;
        case 0x0F: return 9.4248F;
        case 0x10: return 13.0F;
        case 0x12: return 4000.0F;
        case 0x14: return 2000.0F;
        case 0x1D: return 4.0F;
        case 0x30: return 3.0F;
        case 0x33: return 350.0F;
        default: return 0.0F;
    }
}

bool TrueForceDevice::running() const {
    std::lock_guard guard(lock_);
    return running_;
}

TrueForceDevice::Report TrueForceDevice::status(const std::uint8_t type, const std::uint8_t seq,
                                                const std::int64_t nowUs, const std::uint32_t at17) {
    Report r = blank(type, seq);
    putU16(r, 6, countdown_--);
    r[8] = 0x03;
    putU16(r, 9, wireSteering(position_));
    putU16(r, 11, wireSteering(sent_));
    sent_ = position_;
    putU32(r, 13, static_cast<std::uint32_t>(nowUs));
    putU32(r, 17, at17);
    std::copy(std::begin(kStatusTail), std::end(kStatusTail), r.begin() + 21);
    seal(r, 33);
    lastStatusUs_ = nowUs;
    return r;
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
            const float value = parameter(index);
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            putU32(r, 9, bits);
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
            for (const std::uint8_t answerType : {std::uint8_t{0x11}, std::uint8_t{0x14}}) {
                answers.push_back(status(answerType, seq, nowUs, bits));
            }
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

std::optional<TrueForceDevice::Report> TrueForceDevice::steering(const std::int32_t value, const std::int64_t nowUs) {
    const auto clamped = static_cast<std::uint16_t>(std::clamp(value, 0, 65535));
    std::lock_guard guard(lock_);
    position_ = clamped;
    if (!running_ || (lastStatusUs_ >= 0 && nowUs - lastStatusUs_ < kStatusEveryUs)) return std::nullopt;
    return status(0x02, lastSeq_, nowUs);
}

}  // namespace wheelbri
