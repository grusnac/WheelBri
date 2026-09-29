#include "wheelemu/Logging.hpp"

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <filesystem>
#include <mutex>
#include <vector>

namespace wheelemu::logging {

namespace {

// time, level, component, thread, message, source location
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%-5l%$] [%-14n] [tid %-6t] %v  (%s:%#)";

constexpr std::array<const char*, 7> kLevels = {"trace", "debug", "info", "warn", "error", "critical", "off"};

std::mutex gMutex;
std::vector<spdlog::sink_ptr> gSinks;   // guarded by gMutex
bool gInitialised = false;

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
    if (!filePath.empty()) {
        try {
            const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                filePath, config.maxFileSizeBytes, config.maxFiles));
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
        gInitialised = true;
        // Re-point loggers created before init() (e.g. by static initialisers) at the new sinks.
        spdlog::apply_all([&](const Logger& logger) {
            logger->sinks() = sinks;
            logger->set_pattern(kPattern);
        });
    }

    // The registry's formatter is applied to every logger (and its sinks) on registration, so set it globally.
    spdlog::set_pattern(kPattern);

    const auto level = isValidLevel(config.level) ? spdlog::level::from_str(config.level) : spdlog::level::info;
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
        WE_LOG_WARN(logger, "unknown log level '{}', using info", config.level);
    }
    if (!fileOk) {
        WE_LOG_ERROR(logger, "cannot open log file '{}': {} - logging to console only", filePath, fileError);
    } else if (!filePath.empty()) {
        WE_LOG_INFO(logger, "logging to '{}' (level {}, {} files x {} KiB)", filePath,
                    spdlog::level::to_string_view(level), config.maxFiles + 1, config.maxFileSizeBytes / 1024);
    }
    return fileOk;
}

void shutdown() {
    {
        const std::lock_guard<std::mutex> lock(gMutex);
        gInitialised = false;
        gSinks.clear();
    }
    spdlog::shutdown();
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

}  // namespace wheelemu::logging
