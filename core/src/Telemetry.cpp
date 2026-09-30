#include "wheelemu/Telemetry.hpp"

#include <algorithm>
#include <cstdarg>
#include <ctime>
#include <system_error>

namespace wheelemu {

namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint64_t kFlushEveryNs = kNsPerSecond;  // a crash loses at most about a second
constexpr const char* kFormatVersion = "2";

// Appends printf-style text to `out`.
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0) {
        out.append(buffer, static_cast<std::size_t>(std::min<int>(n, static_cast<int>(sizeof(buffer)) - 1)));
    }
}

const char* packetKindName(const FfbPacket::Kind kind) {
    switch (kind) {
        case FfbPacket::Kind::SlotState: return "slot";
        case FfbPacket::Kind::KeepAlive: return "keepalive";
        case FfbPacket::Kind::StopAll: return "stopall";
    }
    return "?";
}

// "constant" -> "constant", "saw up" -> "saw_up": no spaces inside CSV fields.
std::string typeField(const FfbEffectType type) {
    std::string name = ffbTypeName(type);
    std::replace(name.begin(), name.end(), ' ', '_');
    return name;
}

std::string clockText(const WallClock::time_point time) {
    const std::time_t t = WallClock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
}

std::string ffbColumns() {
    return "time,t_ms,kind,slot,seq,playing,type,gain,level,center,deadband,k_pos,k_neg,sat_pos,sat_neg,"
           "magnitude,offset,period_ms,phase_deg";
}

std::string joined(const std::vector<std::string>& columns) {
    std::string line;
    for (const std::string& column : columns) {
        line += (line.empty() ? "" : ",") + column;
    }
    return line;
}

}  // namespace

std::vector<std::string> telemetryColumns() {
    std::vector<std::string> columns = {"time", "t_ms", "seq"};
    for (std::size_t i = 0; i < RawInputState::kMaxAxes; ++i) {
        columns.push_back("raw_a" + std::to_string(i));
    }
    for (const char* name : {"raw_buttons", "raw_hat", "steer", "throttle", "brake", "clutch", "buttons", "hat",
                             "out_steer", "out_throttle", "out_brake", "out_clutch", "out_buttons", "out_hat",
                             "ffb_packets", "ffb_age_ms", "ffb_watchdog", "ffb_plays", "ffb_failures"}) {
        columns.emplace_back(name);
    }
    for (std::size_t slot = 0; slot < IFfbSink::kMaxSlots; ++slot) {
        const std::string prefix = "s" + std::to_string(slot) + "_";
        for (const char* name : {"type", "state", "req", "out"}) {
            columns.push_back(prefix + name);
        }
    }
    columns.emplace_back("period_us");
    columns.emplace_back("work_us");
    return columns;
}

std::unique_ptr<TelemetryRecorder> TelemetryRecorder::open(const TelemetryConfig& config,
                                                           const std::filesystem::path& directory,
                                                           const WallClock::time_point wallNow,
                                                           const std::vector<std::string>& header,
                                                           std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create " + directory.string() + ": " + ec.message();
        return nullptr;
    }
    auto recorder = std::unique_ptr<TelemetryRecorder>(new TelemetryRecorder(config, directory, wallNow, header));
    if (!recorder->openDay(localDate(wallNow), false, error)) {
        return nullptr;
    }
    return recorder;
}

TelemetryRecorder::TelemetryRecorder(const TelemetryConfig& config, std::filesystem::path directory,
                                     const WallClock::time_point sessionStart, std::vector<std::string> header)
    : config_(config),
      directory_(std::move(directory)),
      header_(std::move(header)),
      sessionStart_(sessionStart),
      periodNs_(kNsPerSecond / static_cast<std::uint64_t>(std::clamp(config.rateHz, 1, 1000))) {
    line_.reserve(1024);
}

TelemetryRecorder::~TelemetryRecorder() {
    closeFiles();
}

void TelemetryRecorder::closeFiles() {
    for (File* file : {&samples_, &ffbEvents_}) {
        if (file->handle != nullptr) {
            std::fclose(file->handle);
        }
        *file = File{};
    }
}

bool TelemetryRecorder::openDay(const std::string& day, const bool continued, std::string& error) {
    closeFiles();
    day_ = day;
    dayStart_ = startOfDay(day).value_or(sessionStart_);
    const std::string nextDay = addDays(day, 1).value_or(day);
    nextDayStart_ = startOfDay(nextDay).value_or(dayStart_ + std::chrono::hours(24));
    samplesPath_ = directory_ / ("telemetry-" + day + ".csv");
    ffbPath_ = directory_ / ("ffb-" + day + ".csv");

    std::string sessionHeader = "# session started " + localDate(sessionStart_) + " " + clockText(sessionStart_) +
                                (continued ? ", continued after midnight" : "") + "\n";
    for (const std::string& line : header_) {
        sessionHeader += "# " + line + "\n";
    }

    struct Target {
        File* file;
        const std::filesystem::path* path;
        std::string intro;
    };
    const std::string intro = "# wheelemu telemetry format " + std::string(kFormatVersion) + ", day " + day + "\n";
    const Target targets[] = {
        {&samples_, &samplesPath_,
         intro +
             "# time: local time of day, t_ms: since the session started. raw_*: physical wheel as the OS reports "
             "it (axes -1..1, buttons bit n = button n). steer..hat: after mapping. out_*: values the emulated "
             "wheel reports to the game.\n"
             "# sN_state: w = game wants it playing, p = playing on the wheel. sN_req: what the game asked for, "
             "sN_out: what reached the wheel (signed force for constant, strength for the others).\n" +
             joined(telemetryColumns()) + "\n"},
        {&ffbEvents_, &ffbPath_, intro + ffbColumns() + "\n"},
    };
    for (const Target& target : targets) {
        // Append: every session of the day shares the file. The column row is written once, by the first.
        target.file->handle = std::fopen(target.path->string().c_str(), "ab");
        if (target.file->handle == nullptr) {
            error = "cannot open " + target.path->string();
            closeFiles();
            return false;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(*target.path, ec);
        target.file->bytes = ec ? 0 : static_cast<std::uint64_t>(size);
        if (target.file->bytes == 0) {
            write(*target.file, target.intro);
        }
        write(*target.file, sessionHeader);
        std::fflush(target.file->handle);
    }
    return true;
}

bool TelemetryRecorder::due(const std::uint64_t nowNs) const {
    return !lastSampleNs_ || nowNs - *lastSampleNs_ >= periodNs_;
}

void TelemetryRecorder::write(File& file, const std::string& line) {
    if (file.full || file.handle == nullptr) {
        return;
    }
    const std::uint64_t limit = static_cast<std::uint64_t>(std::max(config_.maxFileMb, 1)) * 1024 * 1024;
    if (file.bytes + line.size() > limit) {
        std::fputs("# stopped: max_file_mb reached\n", file.handle);
        std::fflush(file.handle);
        file.full = true;
        return;
    }
    std::fwrite(line.data(), 1, line.size(), file.handle);
    file.bytes += line.size();
}

void TelemetryRecorder::flushIfDue(const std::uint64_t nowNs) {
    if (nowNs - lastFlushNs_ >= kFlushEveryNs) {
        for (File* file : {&samples_, &ffbEvents_}) {
            if (file->handle != nullptr) {
                std::fflush(file->handle);
            }
        }
        lastFlushNs_ = nowNs;
    }
}

WallClock::time_point TelemetryRecorder::advance(const std::uint64_t nowNs) {
    if (!startNs_) {
        startNs_ = nowNs;
        lastFlushNs_ = nowNs;
    }
    const auto elapsed = std::chrono::nanoseconds(nowNs >= *startNs_ ? nowNs - *startNs_ : 0);
    const WallClock::time_point wall = sessionStart_ + std::chrono::duration_cast<WallClock::duration>(elapsed);
    while (wall >= nextDayStart_) {
        std::string error;
        const std::string nextDay = addDays(day_, 1).value_or(localDate(wall));
        if (!openDay(nextDay, true, error)) {
            break;  // files closed; rows are dropped until the service restarts
        }
    }
    return wall;
}

void TelemetryRecorder::appendTimes(std::string& line, const WallClock::time_point wall,
                                    const std::uint64_t nowNs) const {
    const auto sinceMidnight = std::chrono::duration_cast<std::chrono::milliseconds>(wall - dayStart_).count();
    const long long ms = std::max<long long>(sinceMidnight, 0);
    appendf(line, "%02lld:%02lld:%02lld.%03lld,%.1f", ms / 3'600'000, ms / 60'000 % 60, ms / 1000 % 60, ms % 1000,
            static_cast<double>(nowNs - *startNs_) / 1e6);
}

void TelemetryRecorder::writeSample(const TelemetrySample& sample) {
    const WallClock::time_point wall = advance(sample.nowNs);
    // Keep the grid: the next row is due one period after this one was due, not after it was written.
    lastSampleNs_ = lastSampleNs_ && sample.nowNs - *lastSampleNs_ < 2 * periodNs_ ? *lastSampleNs_ + periodNs_
                                                                                   : sample.nowNs;

    std::string& l = line_;
    l.clear();
    appendTimes(l, wall, sample.nowNs);
    appendf(l, ",%u", static_cast<unsigned>(sample.sequence));
    for (const float axis : sample.raw.axes) {
        appendf(l, ",%.4f", static_cast<double>(axis));
    }
    appendf(l, ",%llx,%d", static_cast<unsigned long long>(sample.raw.buttons), sample.raw.hat);
    appendf(l, ",%.4f,%.4f,%.4f,%.4f,%x,%d", static_cast<double>(sample.state.steering),
            static_cast<double>(sample.state.throttle), static_cast<double>(sample.state.brake),
            static_cast<double>(sample.state.clutch), static_cast<unsigned>(sample.state.buttons), sample.state.hat);
    appendf(l, ",%ld,%ld,%ld,%ld,%x,%d", static_cast<long>(sample.report.steering),
            static_cast<long>(sample.report.throttle), static_cast<long>(sample.report.brake),
            static_cast<long>(sample.report.clutch), static_cast<unsigned>(sample.report.buttons), sample.report.hat);

    const FfbController* const ffb = sample.ffb;
    if (ffb == nullptr) {
        l += ",,,,,";
        for (std::size_t slot = 0; slot < IFfbSink::kMaxSlots; ++slot) {
            l += ",,,,";
        }
    } else {
        const auto& stats = ffb->stats();
        appendf(l, ",%llu,", static_cast<unsigned long long>(stats.packets));
        if (const auto last = ffb->lastPacketNs()) {
            appendf(l, "%.0f", sample.nowNs >= *last ? static_cast<double>(sample.nowNs - *last) / 1e6 : 0.0);
        }
        appendf(l, ",%d,%llu,%llu", ffb->watchdogTripped() ? 1 : 0, static_cast<unsigned long long>(stats.plays),
                static_cast<unsigned long long>(stats.playFailures));
        for (std::size_t slot = 0; slot < IFfbSink::kMaxSlots; ++slot) {
            if (slot >= ffb->slotCount()) {
                l += ",,,,";
                continue;
            }
            const auto view = ffb->slot(slot);
            if (!view.wanted && !view.playing) {
                l += ",,,,";  // idle slots stay empty so active ones stand out
                continue;
            }
            l += ',';
            l += typeField(view.requested.type);
            appendf(l, ",%s%s,%.4f,", view.wanted ? "w" : "", view.playing ? "p" : "",
                    static_cast<double>(ffbStrength(view.requested)));
            if (view.sent) {
                appendf(l, "%.4f", static_cast<double>(ffbStrength(*view.sent)));
            }
        }
    }
    appendf(l, ",%.0f,%.0f\n", sample.periodUs, sample.workUs);

    write(samples_, l);
    ++rows_;
    flushIfDue(sample.nowNs);
}

void TelemetryRecorder::writeFfbPacket(const std::uint64_t nowNs, const FfbPacket& packet) {
    const WallClock::time_point wall = advance(nowNs);
    if (packet.kind == FfbPacket::Kind::KeepAlive) {
        ++keepAlives_;
        return;
    }
    const FfbEffect& e = packet.effect;
    std::string& l = line_;
    l.clear();
    appendTimes(l, wall, nowNs);
    appendf(l, ",%s,%u,%lu,%d,", packetKindName(packet.kind), static_cast<unsigned>(packet.slot),
            static_cast<unsigned long>(packet.sequence), packet.playing ? 1 : 0);
    l += typeField(e.type);
    appendf(l, ",%.4f,%.4f", static_cast<double>(e.gain), static_cast<double>(e.constantLevel));
    appendf(l, ",%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", static_cast<double>(e.condition.center),
            static_cast<double>(e.condition.deadband), static_cast<double>(e.condition.positiveCoefficient),
            static_cast<double>(e.condition.negativeCoefficient), static_cast<double>(e.condition.positiveSaturation),
            static_cast<double>(e.condition.negativeSaturation));
    appendf(l, ",%.4f,%.4f,%.1f,%.1f\n", static_cast<double>(e.periodic.magnitude),
            static_cast<double>(e.periodic.offset), static_cast<double>(e.periodic.periodMs),
            static_cast<double>(e.periodic.phaseDeg));
    write(ffbEvents_, l);
    ++ffbRows_;
    flushIfDue(nowNs);
}

void TelemetryRecorder::noteMalformedFfb(const std::uint64_t nowNs, const std::size_t bytes) {
    const WallClock::time_point wall = advance(nowNs);
    line_.clear();
    appendTimes(line_, wall, nowNs);
    appendf(line_, ",malformed_%lu_bytes,,,,,,,,,,,,,,,,\n", static_cast<unsigned long>(bytes));
    write(ffbEvents_, line_);
}

}  // namespace wheelemu
