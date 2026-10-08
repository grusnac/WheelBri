#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace wheelbri {

// Log levels without spdlog, for code that has none (the drivers) and for the levels the app lets the user pick.
// Header only: the drivers compile it without the rest of core.
//
// The app's Level button (Logs tab) picks how much every component writes: warn (warnings and errors, the default),
// info, debug or trace, in that order on each click and back to warn. Warnings and errors are always written: no
// choice turns them off (clampToApp).
enum class LogLevel : std::uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Critical = 5, Off = 6 };

inline constexpr LogLevel kDefaultAppLogLevel = LogLevel::Warn;

[[nodiscard]] constexpr const char* logLevelName(const LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Critical: return "critical";
        case LogLevel::Off: return "off";
    }
    return "warn";
}

// "trace" ... "off" (as spdlog and the configs name them; "warning" too). Nullopt for anything else.
[[nodiscard]] constexpr std::optional<LogLevel> parseLogLevel(const std::string_view name) {
    if (name == "trace") return LogLevel::Trace;
    if (name == "debug") return LogLevel::Debug;
    if (name == "info") return LogLevel::Info;
    if (name == "warn" || name == "warning") return LogLevel::Warn;
    if (name == "error") return LogLevel::Error;
    if (name == "critical") return LogLevel::Critical;
    if (name == "off") return LogLevel::Off;
    return std::nullopt;
}

// A level the app may set: anything quieter than warn becomes warn (warnings and errors are always written).
[[nodiscard]] constexpr LogLevel clampToApp(const LogLevel level) {
    return static_cast<std::uint8_t>(level) > static_cast<std::uint8_t>(LogLevel::Warn) ? LogLevel::Warn : level;
}

// The app's level from a stored name: a known level, clamped; the default for anything else.
[[nodiscard]] constexpr LogLevel appLogLevel(const std::string_view name) {
    const std::optional<LogLevel> level = parseLogLevel(name);
    return level ? clampToApp(*level) : kDefaultAppLogLevel;
}

// The Level button's next step: warn -> info -> debug -> trace -> warn.
[[nodiscard]] constexpr LogLevel nextAppLogLevel(const LogLevel level) {
    switch (clampToApp(level)) {
        case LogLevel::Warn: return LogLevel::Info;
        case LogLevel::Info: return LogLevel::Debug;
        case LogLevel::Debug: return LogLevel::Trace;
        default: return LogLevel::Warn;
    }
}

// Whether a line of `line`'s level is written when the threshold is `threshold`.
[[nodiscard]] constexpr bool logLevelAllows(const LogLevel threshold, const LogLevel line) {
    return line != LogLevel::Off && static_cast<std::uint8_t>(line) >= static_cast<std::uint8_t>(threshold);
}

}  // namespace wheelbri
