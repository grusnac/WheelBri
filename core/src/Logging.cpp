// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/Logging.hpp"

#include "wheelbri/LogFiles.hpp"

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace wheelbri::logging {

namespace {

// time, level, component, thread, message, source location
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%-5l%$] [%-14n] [tid %-6t] %v  (%s:%#)";

constexpr std::array<const char*, 7> kLevels = {"trace", "debug", "info", "warn", "error", "critical", "off"};

// "wheelbri-service.log" -> "wheelbri-service-2026-09-30-03.log": this run's file of the day (LogFiles.hpp), asked
// for when the sink opens and again at midnight. Should no numbered file be possible, the old daily name.
struct DashedDateFileName {
    static spdlog::filename_t calc_filename(const spdlog::filename_t& filename, const tm& now) {
        char date[40];
        std::snprintf(date, sizeof(date), "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
        const std::filesystem::path path(filename);
        std::string error;
        const std::filesystem::path run =
            logfiles::runFile(path.parent_path(), path.stem().string(), date, path.extension().string(), error);
        return run.empty() ? datedFileName(filename, date) : run.string();
    }
};
using DailySink = spdlog::sinks::daily_file_sink<std::mutex, DashedDateFileName>;

std::mutex gMutex;
std::vector<spdlog::sink_ptr> gSinks;   // guarded by gMutex
std::shared_ptr<DailySink> gFileSink;   // guarded by gMutex
bool gInitialised = false;

// addListener(): the message handed to a function instead of written somewhere.
class ListenerSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ListenerSink(Listener listener) : listener_(std::move(listener)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        listener_(msg.level, std::string(msg.logger_name.data(), msg.logger_name.size()),
                  std::string(msg.payload.data(), msg.payload.size()));
    }
    void flush_() override {}

private:
    Listener listener_;
};

std::vector<spdlog::sink_ptr> fallbackSinks() {
    static const spdlog::sink_ptr stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    return {stderrSink};
}

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

bool isValidLevel(const std::string& level) {
    for (const char* known : kLevels) {
        if (level == known) {
            return true;
        }
    }
    return false;
}

bool init(const LogConfig& config, const std::string& defaultFilePath) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }

    const std::string filePath = config.filePath.empty() ? defaultFilePath : config.filePath;
    bool fileOk = true;
    std::string fileError;
    std::shared_ptr<DailySink> fileSink;
    if (!filePath.empty()) {
        try {
            const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            // Rotates at local midnight on the first message of the new day; old days are handled by the
            // service's [retention] settings, so the sink itself deletes nothing.
            fileSink = std::make_shared<DailySink>(filePath, 0, 0);
            sinks.push_back(fileSink);
        } catch (const std::exception& e) {
            fileOk = false;
            fileError = e.what();
        }
    }
    if (sinks.empty()) {
        sinks = fallbackSinks();  // never lose errors completely
    }

    {
        const std::lock_guard<std::mutex> lock(gMutex);
        gSinks = sinks;
        gFileSink = fileSink;
        gInitialised = true;
        // Re-point loggers created before init() (e.g. by static initialisers) at the new sinks.
        spdlog::apply_all([&](const Logger& logger) {
            logger->sinks() = sinks;
            logger->set_pattern(kPattern);
        });
    }

    // The registry's formatter is applied to every logger (and its sinks) on registration, so set it globally.
    spdlog::set_pattern(kPattern);

    const auto level = isValidLevel(config.level) ? spdlog::level::from_str(config.level) : spdlog::level::warn;
    spdlog::set_level(level);
    spdlog::cfg::load_env_levels();          // SPDLOG_LEVEL overrides the config, per component if wanted
    // Problems reach the file even if the process dies right after logging them.
    spdlog::flush_on(isValidLevel(config.flushLevel) ? spdlog::level::from_str(config.flushLevel)
                                                      : spdlog::level::warn);
    if (config.periodicFlush) {
        spdlog::flush_every(std::chrono::seconds(1));
    }

    const Logger logger = get("logging");
    if (!isValidLevel(config.level)) {
        WE_LOG_WARN(logger, "unknown log level '{}', using warn", config.level);
    }
    if (!fileOk) {
        WE_LOG_ERROR(logger, "cannot open log file '{}': {} - logging to console only", filePath, fileError);
    } else if (!filePath.empty()) {
        WE_LOG_INFO(logger, "logging to '{}' (level {}, a new file every start and every day)", currentLogFile(),
                    spdlog::level::to_string_view(level));
    }
    return fileOk;
}

void addListener(Listener listener) {
    const auto sink = std::make_shared<ListenerSink>(std::move(listener));
    const std::lock_guard<std::mutex> lock(gMutex);
    gSinks.push_back(sink);
    spdlog::apply_all([&](const Logger& logger) { logger->sinks().push_back(sink); });
}

void shutdown() {
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        gInitialised = false;
        gSinks.clear();
        gFileSink.reset();
    }
    spdlog::shutdown();
}

std::string currentLogFile() {
    std::shared_ptr<DailySink> sink;
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        sink = gFileSink;
    }
    return sink != nullptr ? sink->filename() : std::string();
}

std::string datedFileName(const std::string& filePath, const std::string& date) {
    const std::filesystem::path path(filePath);
    std::filesystem::path dated = path.parent_path() / (path.stem().string() + "-" + date + path.extension().string());
    return path.parent_path().empty() ? dated.filename().string() : dated.string();
}

Logger get(const std::string& component) {
    if (Logger existing = spdlog::get(component)) {
        return existing;
    }
    const std::lock_guard<std::mutex> lock(gMutex);
    if (Logger existing = spdlog::get(component)) {  // another thread won the race
        return existing;
    }
    const std::vector<spdlog::sink_ptr> sinks = gInitialised ? gSinks : fallbackSinks();
    auto logger = std::make_shared<spdlog::logger>(component, sinks.begin(), sinks.end());
    spdlog::initialize_logger(logger);  // registers it and applies global/env levels and the global formatter
    if (!gInitialised) {
        logger->set_pattern(kPattern);
        logger->set_level(spdlog::level::warn);
    }
    return logger;
}

bool setLevel(const std::string& level) {
    if (!isValidLevel(level)) {
        return false;
    }
    spdlog::set_level(spdlog::level::from_str(level));
    return true;
}

spdlog::level::level_enum currentLevel() {
    return get("logging")->level();   // looked up each time: shutdown() drops the loggers
}

bool RateLimiter::allow(std::uint64_t& suppressed) {
    const std::int64_t now = nowNs();
    std::int64_t next = nextAllowedNs_.load(std::memory_order_relaxed);
    const auto intervalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(interval_).count();
    if (now >= next && nextAllowedNs_.compare_exchange_strong(next, now + intervalNs, std::memory_order_relaxed)) {
        suppressed = suppressed_.exchange(0, std::memory_order_relaxed);
        return true;
    }
    suppressed_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

std::uint64_t CallSampler::next() {
    const std::uint64_t call = calls_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= firstN_ || (everyN_ != 0 && call % everyN_ == 0)) {
        return call;
    }
    return 0;
}

}  // namespace wheelbri::logging
