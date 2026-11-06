// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppLogLevel.hpp"

#include "wheelbri/ConfigLibrary.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.log");
    return logger;
}

void applyToThisProcess(const LogLevel level) {
    logging::setLevel(logLevelName(level));
}

}  // namespace

AppLogLevel& AppLogLevel::shared() {
    static AppLogLevel* const level = new AppLogLevel();   // never destroyed: other threads may ask until the end
    return *level;
}

void AppLogLevel::start(const std::filesystem::path& stateFile) {
    const std::lock_guard<std::mutex> lock(mutex_);
    stateFile_ = stateFile;
    const LogLevel level = appLogLevel(readAppSetting(stateFile, kSettingKey));
    level_.store(static_cast<std::uint8_t>(level));
    started_.store(true);
    applyToThisProcess(level);
}

LogLevel AppLogLevel::current() const {
    if (started_.load()) return static_cast<LogLevel>(level_.load());
    // spdlog numbers its levels as LogLevel does (trace 0 ... off 6).
    const auto own = static_cast<std::uint8_t>(logging::currentLevel());
    return clampToApp(own <= static_cast<std::uint8_t>(LogLevel::Off) ? static_cast<LogLevel>(own) : LogLevel::Warn);
}

LogLevel AppLogLevel::next() {
    const LogLevel level = nextAppLogLevel(current());
    set(level);
    return level;
}

void AppLogLevel::set(const LogLevel wanted) {
    const LogLevel level = clampToApp(wanted);
    std::vector<Listener> listeners;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto before = static_cast<LogLevel>(level_.load());
        if (started_.load() && before == level) return;
        // Said before a change to warn (the line is still written then), after a change to anything louder.
        const bool quieter = !logLevelAllows(level, LogLevel::Info);
        if (!quieter) applyToThisProcess(level);
        WE_LOG_INFO(log(), "log level {} ({}) for this app, the game's shim, the drivers and the helper",
                    logLevelName(level), quieter ? "warnings and errors only" : "and every level above it");
        if (quieter) applyToThisProcess(level);
        level_.store(static_cast<std::uint8_t>(level));
        started_.store(true);
        changes_.fetch_add(1);
        if (!stateFile_.empty() && !writeAppSetting(stateFile_, kSettingKey, logLevelName(level))) {
            WE_LOG_WARN(log(), "log level {} is not remembered: cannot write {}", logLevelName(level),
                        pathToUtf8(stateFile_));
        }
        listeners = listeners_;
    }
    for (const Listener& listener : listeners) listener(level);
}

void AppLogLevel::addListener(Listener listener) {
    const std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(listener));
}

}  // namespace wheelbri::service
