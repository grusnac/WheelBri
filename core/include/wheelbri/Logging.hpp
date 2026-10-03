#pragma once

// Project-wide logging, built on spdlog.
//
// Conventions:
//  * One named logger per component ("core.net", "service.input", "shim.device", ...), fetched once
//    with logging::get() and kept in a function-local static. The name shows in every line, so a log
//    can be filtered per component.
//  * Always log through the WE_LOG_* macros: they record file:line and are compiled out below
//    SPDLOG_ACTIVE_LEVEL, so trace logging costs nothing in builds that strip it.
//  * Levels: trace = per-tick/per-call detail, debug = decisions and state changes, info = lifecycle
//    (start, device found, config loaded), warn = recoverable problems, error = a feature is broken,
//    critical = the process cannot continue.
//  * Hot paths (1 kHz loop, GetDeviceState, SetParameters) must use RateLimiter or CallSampler.
//  * Levels can be changed without rebuilding: config [logging] level, or per component through the
//    SPDLOG_LEVEL environment variable, e.g. SPDLOG_LEVEL="info,core.net=trace,shim.effect=debug".

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#include "wheelbri/LogConfig.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wheelbri::logging {

using Logger = std::shared_ptr<spdlog::logger>;

// Configures sinks for every logger, existing and future. Call once, early, before starting threads.
// Returns false if the log file could not be opened; console logging still works in that case.
bool init(const LogConfig& config, const std::string& defaultFilePath);

// Full path of the log file being written today, or empty when logging to the console only.
[[nodiscard]] std::string currentLogFile();

// The dated file name used for `filePath` on `date` ("YYYY-MM-DD"): "dir/x.log" -> "dir/x-2026-09-30.log".
[[nodiscard]] std::string datedFileName(const std::string& filePath, const std::string& date);

// Also hands every message (at the loggers' levels) to `listener`: level, component and the bare message, from
// whatever thread logged it. For a window that shows recent log lines. Call after init(), before other threads
// start logging.
using Listener = std::function<void(spdlog::level::level_enum level, const std::string& component,
                                    const std::string& message)>;
void addListener(Listener listener);

// Flushes and releases all sinks. Call before the process (or the DLL) goes away.
void shutdown();

// Returns the logger for a component, creating it on first use. Before init() it writes to stderr.
[[nodiscard]] Logger get(const std::string& component);

// Changes the level of every logger at runtime. Returns false for an unknown level name.
bool setLevel(const std::string& level);

// Allows at most one message per interval and counts what it dropped, so a flood of identical
// problems (e.g. a send failing 1000 times a second) turns into one line every few seconds.
class RateLimiter {
public:
    explicit RateLimiter(std::chrono::milliseconds interval) : interval_(interval) {}

    // True if the caller should log now. `suppressed` receives how many calls were dropped since the last one.
    bool allow(std::uint64_t& suppressed);

private:
    std::chrono::milliseconds interval_;
    std::atomic<std::int64_t> nextAllowedNs_{0};
    std::atomic<std::uint64_t> suppressed_{0};
};

// Logs the first `firstN` calls in full, then every `everyN`-th. For per-call tracing of chatty APIs.
class CallSampler {
public:
    CallSampler(std::uint64_t firstN, std::uint64_t everyN) : firstN_(firstN), everyN_(everyN) {}

    // Returns the 1-based call number when this call should be logged, or 0 when it should not.
    std::uint64_t next();

private:
    std::uint64_t firstN_;
    std::uint64_t everyN_;
    std::atomic<std::uint64_t> calls_{0};
};

}  // namespace wheelbri::logging

#define WE_LOG_TRACE(logger, ...) SPDLOG_LOGGER_TRACE(logger, __VA_ARGS__)
#define WE_LOG_DEBUG(logger, ...) SPDLOG_LOGGER_DEBUG(logger, __VA_ARGS__)
#define WE_LOG_INFO(logger, ...) SPDLOG_LOGGER_INFO(logger, __VA_ARGS__)
#define WE_LOG_WARN(logger, ...) SPDLOG_LOGGER_WARN(logger, __VA_ARGS__)
#define WE_LOG_ERROR(logger, ...) SPDLOG_LOGGER_ERROR(logger, __VA_ARGS__)
#define WE_LOG_CRITICAL(logger, ...) SPDLOG_LOGGER_CRITICAL(logger, __VA_ARGS__)

// Rate-limited variants: `limiter` is a logging::RateLimiter that lives as long as the call site
// (usually a function-local static). The number of dropped messages is appended.
#define WE_LOG_WARN_LIMITED(logger, limiter, fmt, ...)                                         \
    do {                                                                                      \
        std::uint64_t we_suppressed_ = 0;                                                     \
        if ((limiter).allow(we_suppressed_)) {                                                \
            SPDLOG_LOGGER_WARN(logger, fmt " ({} similar suppressed)" __VA_OPT__(, ) __VA_ARGS__, we_suppressed_); \
        }                                                                                     \
    } while (false)

#define WE_LOG_ERROR_LIMITED(logger, limiter, fmt, ...)                                        \
    do {                                                                                      \
        std::uint64_t we_suppressed_ = 0;                                                     \
        if ((limiter).allow(we_suppressed_)) {                                                \
            SPDLOG_LOGGER_ERROR(logger, fmt " ({} similar suppressed)" __VA_OPT__(, ) __VA_ARGS__, we_suppressed_); \
        }                                                                                     \
    } while (false)
