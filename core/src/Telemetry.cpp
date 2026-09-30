#include "wheelemu/Telemetry.hpp"

#include <algorithm>
#include <cstdarg>
#include <system_error>

namespace wheelemu {

namespace {

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;
constexpr std::uint64_t kFlushEveryNs = kNsPerSecond;  // a crash loses at most about a second
constexpr const char* kFormatVersion = "1";

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

double msSince(const std::uint64_t startNs, const std::uint64_t nowNs) {
    return nowNs >= startNs ? static_cast<double>(nowNs - startNs) / 1e6 : 0.0;
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

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const char* ffbColumns() {
    return "t_ms,kind,slot,seq,playing,type,gain,level,center,deadband,k_pos,k_neg,sat_pos,sat_neg,"
           "magnitude,offset,period_ms,phase_deg\n";
}

}  // namespace

std::vector<std::string> telemetryColumns() {
    std::vector<std::string> columns = {"t_ms", "seq"};
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

std::string telemetryStamp(const std::time_t time) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

std::size_t pruneTelemetry(const std::filesystem::path& directory, const int keep) {
    std::error_code ec;
    std::vector<std::string> stamps;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (startsWith(name, "telemetry-") && endsWith(name, ".csv")) {
            stamps.push_back(name.substr(10, name.size() - 10 - 4));
        }
    }
    // Stamps sort by time; a session with only an ffb-*.csv left is ignored (it is deleted with its pair).
    std::sort(stamps.begin(), stamps.end());
    std::size_t deleted = 0;
    const std::size_t keepCount = static_cast<std::size_t>(std::max(keep, 0));
    for (std::size_t i = 0; i + keepCount < stamps.size(); ++i) {
        for (const std::string& prefix : {std::string("telemetry-"), std::string("ffb-")}) {
            if (std::filesystem::remove(directory / (prefix + stamps[i] + ".csv"), ec)) {
                ++deleted;
            }
        }
    }
    return deleted;
}

std::unique_ptr<TelemetryRecorder> TelemetryRecorder::open(const TelemetryConfig& config,
                                                           const std::filesystem::path& directory,
                                                           const std::string& stamp,
                                                           const std::vector<std::string>& header,
                                                           std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create " + directory.string() + ": " + ec.message();
        return nullptr;
    }
    auto recorder = std::unique_ptr<TelemetryRecorder>(new TelemetryRecorder(
        config, directory / ("telemetry-" + stamp + ".csv"), directory / ("ffb-" + stamp + ".csv")));
    recorder->samples_.handle = std::fopen(recorder->samplesPath_.string().c_str(), "wb");
    recorder->ffbEvents_.handle = std::fopen(recorder->ffbPath_.string().c_str(), "wb");
    if (recorder->samples_.handle == nullptr || recorder->ffbEvents_.handle == nullptr) {
        error = "cannot create telemetry files in " + directory.string();
        return nullptr;
    }

    std::string comments = "# wheelemu telemetry format " + std::string(kFormatVersion) + ", session " + stamp + "\n";
    for (const std::string& line : header) {
        comments += "# " + line + "\n";
    }
    std::string columns;
    for (const std::string& column : telemetryColumns()) {
        columns += (columns.empty() ? "" : ",") + column;
    }
    recorder->write(recorder->samples_,
                    comments +
                        "# raw_*: physical wheel as the OS reports it (axes -1..1, buttons bit n = button n). "
                        "steer..hat: after mapping. out_*: values the emulated wheel reports to the game.\n"
                        "# sN_state: w = game wants it playing, p = playing on the wheel. sN_req: what the game "
                        "asked for, sN_out: what reached the wheel (signed force for constant, strength for "
                        "the others).\n" +
                        columns + "\n");
    recorder->write(recorder->ffbEvents_, comments + ffbColumns());
    std::fflush(recorder->samples_.handle);
    std::fflush(recorder->ffbEvents_.handle);
    return recorder;
}

TelemetryRecorder::TelemetryRecorder(const TelemetryConfig& config, std::filesystem::path samplesPath,
                                     std::filesystem::path ffbPath)
    : config_(config),
      samplesPath_(std::move(samplesPath)),
      ffbPath_(std::move(ffbPath)),
      periodNs_(kNsPerSecond / static_cast<std::uint64_t>(std::clamp(config.rateHz, 1, 1000))) {
    line_.reserve(1024);
}

TelemetryRecorder::~TelemetryRecorder() {
    for (File* file : {&samples_, &ffbEvents_}) {
        if (file->handle != nullptr) {
            std::fclose(file->handle);
        }
    }
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
        const char* const note = "# stopped: max_file_mb reached\n";
        std::fputs(note, file.handle);
        std::fflush(file.handle);
        file.full = true;
        return;
    }
    std::fwrite(line.data(), 1, line.size(), file.handle);
    file.bytes += line.size();
}

void TelemetryRecorder::flushIfDue(const std::uint64_t nowNs) {
    if (nowNs - lastFlushNs_ >= kFlushEveryNs) {
        std::fflush(samples_.handle);
        std::fflush(ffbEvents_.handle);
        lastFlushNs_ = nowNs;
    }
}

void TelemetryRecorder::writeSample(const TelemetrySample& sample) {
    if (!startNs_) {
        startNs_ = sample.nowNs;
        lastFlushNs_ = sample.nowNs;
    }
    // Keep the grid: the next row is due one period after this one was due, not after it was written.
    lastSampleNs_ = lastSampleNs_ && sample.nowNs - *lastSampleNs_ < 2 * periodNs_ ? *lastSampleNs_ + periodNs_
                                                                                   : sample.nowNs;

    std::string& l = line_;
    l.clear();
    appendf(l, "%.1f,%u", msSince(*startNs_, sample.nowNs), static_cast<unsigned>(sample.sequence));
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
            appendf(l, "%.0f", msSince(*last, sample.nowNs));
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
    if (!startNs_) {
        startNs_ = nowNs;
        lastFlushNs_ = nowNs;
    }
    if (packet.kind == FfbPacket::Kind::KeepAlive) {
        ++keepAlives_;
        return;
    }
    const FfbEffect& e = packet.effect;
    std::string& l = line_;
    l.clear();
    appendf(l, "%.1f,%s,%u,%lu,%d,", msSince(*startNs_, nowNs), packetKindName(packet.kind),
            static_cast<unsigned>(packet.slot), static_cast<unsigned long>(packet.sequence), packet.playing ? 1 : 0);
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
    if (!startNs_) {
        startNs_ = nowNs;
        lastFlushNs_ = nowNs;
    }
    line_.clear();
    appendf(line_, "%.1f,malformed_%lu_bytes,,,,,,,,,,,,,,,,\n", msSince(*startNs_, nowNs),
            static_cast<unsigned long>(bytes));
    write(ffbEvents_, line_);
}

}  // namespace wheelemu
