// SPDX-License-Identifier: GPL-3.0-or-later
#include "LogiLogging.hpp"

#include "LogFolder.hpp"
#include "Settings.hpp"

#include "wheelbri/LogLevel.hpp"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace wheelbri::logi {

namespace {

std::once_flag gInitOnce;
std::atomic<bool> gInitialised{false};

}  // namespace

void ensureLoggingInitialised() {
    std::call_once(gInitOnce, [] {
        logging::LogConfig config;
        const Settings& own = settings();
        config.level = own.logLevel.empty() ? std::string(logLevelName(gamelog::appLevelAtStart())) : own.logLevel;
        config.console = false;
        config.flushLevel = "trace";
        config.periodicFlush = false;
        logging::init(config, gamelog::logFilePath(L"wheelbri-logi.log"));
        gInitialised = true;

        const logging::Logger log = logging::get("logi");
        WE_LOG_INFO(log, "wheelbri-logi.dll (Logitech wheel SDK stand-in) loaded into '{}' (pid {})",
                    gamelog::utf8(gamelog::modulePath(nullptr)), GetCurrentProcessId());
        WE_LOG_INFO(log, "this dll: '{}', log level {} ({})", gamelog::utf8(gamelog::modulePath(gamelog::thisModule())),
                    config.level, own.logLevel.empty() ? "the app's" : "wheelbri-logi.ini");
        for (const std::string& problem : own.problems) {
            WE_LOG_WARN(log, "{}", problem);
        }
#if defined(_WIN64)
        WE_LOG_INFO(log, "architecture: x64");
#else
        WE_LOG_INFO(log, "architecture: x86");
#endif
        if (std::string wine; gamelog::underWine(wine)) {
            WE_LOG_INFO(log, "running under Wine/Proton {}", wine);
        }
    });
}

void onDllDetach(const bool processTerminating) noexcept {
    if (processTerminating || !gInitialised.load()) {
        return;
    }
    try {
        const logging::Logger log = logging::get("logi");
        WE_LOG_INFO(log, "unloaded by FreeLibrary");
        logging::shutdown();
    } catch (...) {
    }
}

logging::Logger logger(const char* component) {
    ensureLoggingInitialised();
    return logging::get(component);
}

void noteEscapedException(const char* where, const char* what) noexcept {
    try {
        static const logging::Logger instance = logger("logi");
        static logging::RateLimiter limiter(std::chrono::seconds(5));
        WE_LOG_ERROR_LIMITED(instance, limiter, "{}: an error in the logging, the game's call went through: {}", where,
                             what != nullptr ? what : "?");
    } catch (...) {
    }
}

}  // namespace wheelbri::logi
