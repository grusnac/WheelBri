#include "ShimLogging.hpp"

#include "Guard.hpp"

#include "LogFolder.hpp"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace wheelbri::shim {

namespace {

std::once_flag gInitOnce;
// Set once logging::init has run. onDllDetach checks it so an unload before the first export call never
// creates a logger inside DllMain.
std::atomic<bool> gInitialised{false};
std::atomic<std::uint8_t> gLevel{static_cast<std::uint8_t>(kDefaultAppLogLevel)};   // what applyLogLevel last set

}  // namespace

void ensureLoggingInitialised() {
    std::call_once(gInitOnce, [] {
        logging::LogConfig config;
        config.level = logLevelName(gamelog::appLevelAtStart());
        config.console = false;
        config.flushLevel = "trace";
        config.periodicFlush = false;
        logging::init(config, gamelog::logFilePath(L"wheelbri-shim.log"));
        gLevel = static_cast<std::uint8_t>(appLogLevel(config.level));
        gInitialised = true;

        const logging::Logger log = logging::get("shim");
        WE_LOG_INFO(log, "wheelbri dinput8 proxy loaded into '{}' (pid {})",
                    gamelog::utf8(gamelog::modulePath(nullptr)), GetCurrentProcessId());
        WE_LOG_INFO(log, "proxy dll: '{}'", gamelog::utf8(gamelog::modulePath(gamelog::thisModule())));
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
    // Never logged: nothing to flush, and creating a logger here would take locks and open files in DllMain.
    if (processTerminating || !gInitialised.load()) {
        return;
    }
    try {   // nothing may throw out of DllMain
        const logging::Logger log = logging::get("shim");
        WE_LOG_INFO(log, "proxy unloaded by FreeLibrary");
        logging::shutdown();
    } catch (...) {
    }
}

logging::Logger logger(const char* component) {
    ensureLoggingInitialised();
    return logging::get(component);
}

// Guard.hpp: an exception stopped at the boundary to the game.
void noteEscapedException(const char* where, const char* what) noexcept {
    try {
        static const logging::Logger instance = logger("shim");
        static logging::RateLimiter limiter(std::chrono::seconds(5));
        WE_LOG_ERROR_LIMITED(instance, limiter, "{}: stopped on an error, the game gets an error code: {}", where,
                             what != nullptr ? what : "?");
    } catch (...) {
        // Logging itself failed (out of memory?): nothing more can be done safely.
    }
}

void applyLogLevel(const LogLevel level) {
    ensureLoggingInitialised();   // first: it sets the level it starts with
    if (gLevel.exchange(static_cast<std::uint8_t>(level)) == static_cast<std::uint8_t>(level)) return;
    const logging::Logger log = logging::get("shim");
    const bool quieter = !logLevelAllows(level, LogLevel::Info);
    if (!quieter) logging::setLevel(logLevelName(level));
    WE_LOG_INFO(log, "log level {} (the app's Level button)", logLevelName(level));
    if (quieter) logging::setLevel(logLevelName(level));
}

}  // namespace wheelbri::shim
