#include "wheelbri/Telemetry.hpp"

#include "wheelbri/LogFiles.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <system_error>

namespace wheelbri {

namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint64_t kFlushEveryNs = kNsPerSecond;  // a crash loses at most about a second
constexpr const char* kFormatVersion = "3";   // 3: log lines with key=value (2: CSV)
// The component field of the lines, padded as spdlog pads the other logs' (%-14n).
constexpr const char* kSamplesComponent = "telemetry     ";
constexpr const char* kFfbComponent = "ffb           ";
constexpr auto kRetryEvery = std::chrono::minutes(1);

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.telemetry");
    return logger;
}

// UTF-8 text of a path, for messages (path::string() can throw on Windows for names outside the ANSI code page).
std::string text(const std::filesystem::path& path) {
    try {
        const std::u8string name = path.u8string();
        return std::string(name.begin(), name.end());
    } catch (...) {
        return "(unprintable path)";
    }
}

// Opens for appending. On Windows by its wide name, so a profile folder with any letters in it works.
std::FILE* openAppend(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

// Appends printf-formatted text. Short pieces go through a stack buffer; a longer one (extreme numbers: a float near
// 3.4e38 prints as about 45 characters) is formatted again at its full length rather than cut.
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(buffer)) {
        out.append(buffer, static_cast<std::size_t>(n));
    } else if (n > 0) {
        const std::size_t at = out.size();
        out.resize(at + static_cast<std::size_t>(n) + 1);
        std::vsnprintf(out.data() + at, static_cast<std::size_t>(n) + 1, format, again);
        out.resize(at + static_cast<std::size_t>(n));
    }
    va_end(again);
}

const char* packetKindName(const FfbPacket::Kind kind) {
    switch (kind) {
        case FfbPacket::Kind::SlotState: return "slot";
        case FfbPacket::Kind::KeepAlive: return "keepalive";
        case FfbPacket::Kind::StopAll: return "stopall";
    }
    return "?";
}

// "constant" -> "constant", "saw up" -> "saw_up": no spaces inside a value.
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
        error = "cannot create " + text(directory) + ": " + ec.message();
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
    forceRow_ = true;
    lastSlots_.fill(std::nullopt);
    day_ = day;
    dayStart_ = startOfDay(day).value_or(sessionStart_);
    const std::string nextDay = addDays(day, 1).value_or(day);
    nextDayStart_ = startOfDay(nextDay).value_or(dayStart_ + std::chrono::hours(24));
    // This run's files of the day, numbered like the app's own log (LogFiles.hpp): "telemetry-2026-10-08-03.log".
    std::string nameError;
    samplesPath_ = logfiles::runFile(directory_, "telemetry", day, ".log", nameError);
    if (!samplesPath_.empty()) ffbPath_ = logfiles::runFile(directory_, "ffb", day, ".log", nameError);
    if (samplesPath_.empty() || ffbPath_.empty()) {
        error = nameError;
        closeFiles();
        dayOpen_ = false;
        return false;
    }

    // The lines before the rows carry the time the session (or, after midnight, the day) started.
    const WallClock::time_point headerTime = continued || sessionStart_ < dayStart_ ? dayStart_ : sessionStart_;
    struct Target {
        File* file;
        const std::filesystem::path* path;
        const char* component;
        std::vector<std::string> intro;   // a new file's first lines
    };
    const std::string format = "wheelbri telemetry format " + std::string(kFormatVersion) + ", day " + day +
                               ": one line per row, values as key=value; a key that is missing has no value then";
    const Target targets[] = {
        {&samples_, &samplesPath_, kSamplesComponent,
         {format,
          "keys: t_ms = since the session started; raw_* = the physical wheel as the OS reports it (axes -1..1, "
          "buttons bit n = button n, hex); steer..hat = after mapping; out_* = what the emulated wheel reports to the "
          "game",
          "keys: sN_* = force feedback slot N while it is used: sN_state w = the game wants it playing, p = playing "
          "on the wheel; sN_req = what the game asked for, sN_out = what reached the wheel (signed force for "
          "constant, strength for the others); period_us / work_us = the loop"}},
        {&ffbEvents_, &ffbPath_, kFfbComponent,
         {format,
          "keys: kind slot / stopall = what the game sent (every effect parameter); kind wheel_play / wheel_stop / "
          "wheel_stopall = a call made to the wheel: ok = it accepted it, wait_us = queued before the call, call_us "
          "= the wheel's driver; wheel slot 8 = the target's centring spring (WheelBri's own, not the game's)"}},
    };
    std::string line;
    for (const Target& target : targets) {
        // Append: every session of this run shares the file; a new file starts with what its keys mean.
        target.file->handle = openAppend(*target.path);
        if (target.file->handle == nullptr) {
            error = "cannot open " + text(*target.path) + " (" + std::strerror(errno) + ")";
            closeFiles();
            dayOpen_ = false;
            return false;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(*target.path, ec);
        target.file->bytes = ec ? 0 : static_cast<std::uint64_t>(size);
        std::vector<std::string> messages;
        if (target.file->bytes == 0) messages = target.intro;
        messages.push_back("session started " + localDate(sessionStart_) + " " + clockText(sessionStart_) +
                           (continued ? ", continued after midnight" : ""));
        messages.insert(messages.end(), header_.begin(), header_.end());
        for (const std::string& message : messages) {
            line.clear();
            appendStamp(line, headerTime, target.component);
            line += message;
            line += '\n';
            write(*target.file, line);
        }
        if (std::fflush(target.file->handle) != 0) noteWriteFailure(*target.file, "writing its header");
    }
    dayOpen_ = true;
    return true;
}

void TelemetryRecorder::noteWriteFailure(File& file, const char* what) {
    if (file.failed) return;
    file.failed = true;
    WE_LOG_ERROR(log(), "telemetry: {} failed while {} ({}): nothing more is written to it today (disk full?)",
                 text(&file == &samples_ ? samplesPath_ : ffbPath_), what, std::strerror(errno));
}

bool TelemetryRecorder::due(const std::uint64_t nowNs) const {
    return !lastSampleNs_ || nowNs - *lastSampleNs_ >= periodNs_;
}

void TelemetryRecorder::write(File& file, const std::string& line) {
    if (file.full || file.failed || file.handle == nullptr) {
        return;
    }
    const std::uint64_t limit = static_cast<std::uint64_t>(std::max(config_.maxFileMb, 1)) * 1024 * 1024;
    if (file.bytes + line.size() > limit) {
        const std::string note = lastStamp_ + "stopped: max_file_mb reached\n";
        std::fputs(note.c_str(), file.handle);
        std::fflush(file.handle);
        file.full = true;
        return;
    }
    if (std::fwrite(line.data(), 1, line.size(), file.handle) != line.size()) {
        noteWriteFailure(file, "writing a row");
        return;
    }
    file.bytes += line.size();
}

void TelemetryRecorder::flushIfDue(const std::uint64_t nowNs) {
    if (nowNs - lastFlushNs_ >= kFlushEveryNs) {
        for (File* file : {&samples_, &ffbEvents_}) {
            if (file->handle != nullptr && !file->failed && std::fflush(file->handle) != 0) {
                noteWriteFailure(*file, "flushing");
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
            // Rows are dropped meanwhile; tried again below, once a minute.
            WE_LOG_ERROR(log(), "telemetry: the new day's files cannot be opened: {}; trying again every minute",
                         error);
            retryAt_ = wall + kRetryEvery;
            break;
        }
    }
    if (!dayOpen_ && wall >= retryAt_ && wall < nextDayStart_) {
        std::string error;
        if (openDay(day_, true, error)) {
            WE_LOG_INFO(log(), "telemetry: writing again to {}", text(samplesPath_));
        } else {
            static logging::RateLimiter limiter(std::chrono::minutes(10));
            WE_LOG_WARN_LIMITED(log(), limiter, "telemetry: still cannot open the day's files: {}", error);
            retryAt_ = wall + kRetryEvery;
        }
    }
    return wall;
}

void TelemetryRecorder::appendStamp(std::string& line, const WallClock::time_point wall, const char* component) {
    const auto sinceMidnight = std::chrono::duration_cast<std::chrono::milliseconds>(wall - dayStart_).count();
    const long long ms = std::max<long long>(sinceMidnight, 0);
    const std::size_t start = line.size();
    appendf(line, "[%s %02lld:%02lld:%02lld.%03lld] [info ] [%s] ", day_.c_str(), ms / 3'600'000, ms / 60'000 % 60,
            ms / 1000 % 60, ms % 1000, component);
    lastStamp_.assign(line, start, std::string::npos);
}

void TelemetryRecorder::appendTimes(std::string& line, const WallClock::time_point wall, const std::uint64_t nowNs,
                                    const char* component) {
    appendStamp(line, wall, component);
    // A time before the first one (as advance() takes it) is 0, never an unsigned wrap-around.
    const std::uint64_t sinceStart = nowNs >= *startNs_ ? nowNs - *startNs_ : 0;
    appendf(line, "t_ms=%.1f", static_cast<double>(sinceStart) / 1e6);
}

bool TelemetryRecorder::changed(const TelemetrySample& sample, const std::string& exact) const {
    if (exact != lastExact_) {
        return true;
    }
    const float min = std::max(config_.minChange, 1e-6F);
    const auto moved = [min](const float a, const float b) { return std::abs(a - b) >= min; };
    for (std::size_t i = 0; i < sample.raw.axes.size(); ++i) {
        if (moved(sample.raw.axes[i], lastRaw_.axes[i])) {
            return true;
        }
    }
    const WheelState& s = sample.state;
    if (moved(s.steering, lastState_.steering) || moved(s.throttle, lastState_.throttle) ||
        moved(s.brake, lastState_.brake) || moved(s.clutch, lastState_.clutch)) {
        return true;
    }
    // Emulated wheel units: the same threshold on a 16-bit range, at least one count.
    const std::int64_t minCounts = std::max<std::int64_t>(1, static_cast<std::int64_t>(min * 32768.0F));
    const auto movedCounts = [minCounts](const std::int32_t a, const std::int32_t b) {
        return std::llabs(static_cast<long long>(a) - static_cast<long long>(b)) >= minCounts;
    };
    const TargetReport& r = sample.report;
    return movedCounts(r.steering, lastReport_.steering) || movedCounts(r.throttle, lastReport_.throttle) ||
           movedCounts(r.brake, lastReport_.brake) || movedCounts(r.clutch, lastReport_.clutch);
}

void TelemetryRecorder::writeSample(const TelemetrySample& sample) {
    const WallClock::time_point wall = advance(sample.nowNs);
    // Keep the grid: the next row is due one period after this one was due, not after it was written.
    lastSampleNs_ = lastSampleNs_ && sample.nowNs - *lastSampleNs_ < 2 * periodNs_ ? *lastSampleNs_ + periodNs_
                                                                                   : sample.nowNs;

    // Row values. Times, sequence numbers, the packet counter, packet age and loop timing change all the
    // time and never make a row on their own.
    // Reused buffers (members): no allocation per sample once they have grown (the loop may pass 2000 times a second).
    std::string& k = sampleValues_;
    k.clear();
    for (std::size_t i = 0; i < sample.raw.axes.size(); ++i) {
        appendf(k, " raw_a%u=%.4f", static_cast<unsigned>(i), static_cast<double>(sample.raw.axes[i]));
    }
    appendf(k, " raw_buttons=%llx raw_hat=%d", static_cast<unsigned long long>(sample.raw.buttons), sample.raw.hat);
    appendf(k, " steer=%.4f throttle=%.4f brake=%.4f clutch=%.4f buttons=%x hat=%d",
            static_cast<double>(sample.state.steering), static_cast<double>(sample.state.throttle),
            static_cast<double>(sample.state.brake), static_cast<double>(sample.state.clutch),
            static_cast<unsigned>(sample.state.buttons), sample.state.hat);
    appendf(k, " out_steer=%ld out_throttle=%ld out_brake=%ld out_clutch=%ld out_buttons=%x out_hat=%d",
            static_cast<long>(sample.report.steering), static_cast<long>(sample.report.throttle),
            static_cast<long>(sample.report.brake), static_cast<long>(sample.report.clutch),
            static_cast<unsigned>(sample.report.buttons), sample.report.hat);

    const FfbController* const ffb = sample.ffb;
    std::string& slots = sampleSlots_;
    slots.clear();
    if (ffb != nullptr) {
        for (std::size_t slot = 0; slot < IFfbSink::kMaxSlots && slot < ffb->slotCount(); ++slot) {
            const auto view = ffb->slot(slot);
            if (!view.wanted && !view.playing) continue;   // idle slots are left out so active ones stand out
            const auto n = static_cast<unsigned>(slot);
            appendf(slots, " s%u_type=", n);
            slots += typeField(view.requested.type);
            appendf(slots, " s%u_state=%s%s s%u_req=%.4f", n, view.wanted ? "w" : "", view.playing ? "p" : "", n,
                    static_cast<double>(ffbStrength(view.requested)));
            if (view.sent) {
                appendf(slots, " s%u_out=%.4f", n, static_cast<double>(ffbStrength(*view.sent)));
            }
        }
    }
    // Compared exactly: buttons, hats and everything force feedback (the game's forces are the output we
    // are here to see, so every change of them counts).
    std::string& exact = sampleExact_;
    exact.clear();
    appendf(exact, "%llx,%d,%x,%d,%x,%d", static_cast<unsigned long long>(sample.raw.buttons), sample.raw.hat,
            static_cast<unsigned>(sample.state.buttons), sample.state.hat, static_cast<unsigned>(sample.report.buttons),
            sample.report.hat);
    if (ffb != nullptr) {
        appendf(exact, "|%d,%llu", ffb->watchdogTripped() ? 1 : 0,
                static_cast<unsigned long long>(ffb->stats().playFailures));
    }
    exact += slots;

    if (config_.onlyChanges && !forceRow_ && !changed(sample, exact)) {
        const std::uint64_t heartbeatNs = static_cast<std::uint64_t>(std::max(config_.heartbeatSeconds, 0)) * kNsPerSecond;
        if (heartbeatNs == 0 || !lastWrittenNs_ || sample.nowNs - *lastWrittenNs_ < heartbeatNs) {
            ++skipped_;
            return;
        }
    }
    lastExact_.swap(exact);   // `exact` keeps the old buffer for the next sample
    lastRaw_ = sample.raw;
    lastState_ = sample.state;
    lastReport_ = sample.report;
    lastWrittenNs_ = sample.nowNs;
    forceRow_ = false;

    std::string& l = line_;
    l.clear();
    appendTimes(l, wall, sample.nowNs, kSamplesComponent);
    appendf(l, " seq=%u", static_cast<unsigned>(sample.sequence));
    l += k;
    if (ffb != nullptr) {
        const auto& stats = ffb->stats();
        appendf(l, " ffb_packets=%llu", static_cast<unsigned long long>(stats.packets));
        if (const auto last = ffb->lastPacketNs()) {
            appendf(l, " ffb_age_ms=%.0f",
                    sample.nowNs >= *last ? static_cast<double>(sample.nowNs - *last) / 1e6 : 0.0);
        }
        appendf(l, " ffb_watchdog=%d ffb_plays=%llu ffb_failures=%llu", ffb->watchdogTripped() ? 1 : 0,
                static_cast<unsigned long long>(stats.plays), static_cast<unsigned long long>(stats.playFailures));
    }
    l += slots;
    appendf(l, " period_us=%.0f work_us=%.0f\n", sample.periodUs, sample.workUs);

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
    // Only real changes: the shim re-sends every slot four times a second so a lost datagram heals.
    if (packet.kind == FfbPacket::Kind::StopAll) {
        lastSlots_.fill(std::nullopt);
    } else if (packet.slot < lastSlots_.size()) {
        auto& last = lastSlots_[packet.slot];
        const std::pair<bool, FfbEffect> now{packet.playing, packet.effect};
        if (last && *last == now) {
            return;
        }
        last = now;
    }
    const FfbEffect& e = packet.effect;
    std::string& l = line_;
    l.clear();
    appendTimes(l, wall, nowNs, kFfbComponent);
    appendf(l, " kind=%s seq=%lu", packetKindName(packet.kind), static_cast<unsigned long>(packet.sequence));
    if (packet.kind == FfbPacket::Kind::SlotState) {
        appendf(l, " slot=%u playing=%d type=", static_cast<unsigned>(packet.slot), packet.playing ? 1 : 0);
        l += typeField(e.type);
        appendf(l, " gain=%.4f level=%.4f", static_cast<double>(e.gain), static_cast<double>(e.constantLevel));
        appendf(l, " center=%.4f deadband=%.4f k_pos=%.4f k_neg=%.4f sat_pos=%.4f sat_neg=%.4f",
                static_cast<double>(e.condition.center), static_cast<double>(e.condition.deadband),
                static_cast<double>(e.condition.positiveCoefficient),
                static_cast<double>(e.condition.negativeCoefficient),
                static_cast<double>(e.condition.positiveSaturation),
                static_cast<double>(e.condition.negativeSaturation));
        appendf(l, " magnitude=%.4f offset=%.4f period_ms=%.1f phase_deg=%.1f",
                static_cast<double>(e.periodic.magnitude),
                static_cast<double>(e.periodic.offset), static_cast<double>(e.periodic.periodMs),
                static_cast<double>(e.periodic.phaseDeg));
    }
    l += '\n';
    write(ffbEvents_, l);
    ++ffbRows_;
    flushIfDue(nowNs);
}

void TelemetryRecorder::writeFfbApplied(const std::uint64_t nowNs, const AsyncFfbSink::Applied& applied) {
    const WallClock::time_point wall = advance(nowNs);
    const char* kind = applied.kind == AsyncFfbSink::Applied::Kind::Play   ? "wheel_play"
                       : applied.kind == AsyncFfbSink::Applied::Kind::Stop ? "wheel_stop"
                                                                             : "wheel_stopall";
    std::string& l = line_;
    l.clear();
    appendTimes(l, wall, nowNs, kFfbComponent);
    appendf(l, " kind=%s", kind);
    if (applied.kind != AsyncFfbSink::Applied::Kind::StopAll) {
        appendf(l, " slot=%u", static_cast<unsigned>(applied.slot));
    }
    appendf(l, " ok=%d", applied.ok ? 1 : 0);
    if (applied.kind == AsyncFfbSink::Applied::Kind::Play) {
        l += " type=";
        l += typeField(applied.type);
    }
    appendf(l, " wait_us=%lu call_us=%lu\n", static_cast<unsigned long>(applied.waitUs),
            static_cast<unsigned long>(applied.callUs));
    write(ffbEvents_, l);
    ++ffbRows_;
    flushIfDue(nowNs);
}

void TelemetryRecorder::noteMalformedFfb(const std::uint64_t nowNs, const std::size_t bytes) {
    // At most one row a second: a stray program flooding the port must not fill the file (or the disk).
    if (lastMalformedNs_ != 0 && nowNs - lastMalformedNs_ < kNsPerSecond) {
        ++malformedSkipped_;
        return;
    }
    lastMalformedNs_ = nowNs;
    const WallClock::time_point wall = advance(nowNs);
    line_.clear();
    appendTimes(line_, wall, nowNs, kFfbComponent);
    appendf(line_, " kind=malformed bytes=%lu skipped_before=%llu\n", static_cast<unsigned long>(bytes),
            static_cast<unsigned long long>(malformedSkipped_));
    malformedSkipped_ = 0;
    write(ffbEvents_, line_);
}

}  // namespace wheelbri
