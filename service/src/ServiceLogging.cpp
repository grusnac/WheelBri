#include "ServiceLogging.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>

#ifndef WHEELEMU_VERSION
#define WHEELEMU_VERSION "dev"
#endif

namespace wheelemu::service {

namespace {

std::string envOrEmpty(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? value : "";
}

spdlog::level::level_enum toSpdlogLevel(const SDL_LogPriority priority) {
    switch (priority) {
        case SDL_LOG_PRIORITY_TRACE:
        case SDL_LOG_PRIORITY_VERBOSE: return spdlog::level::trace;
        case SDL_LOG_PRIORITY_DEBUG: return spdlog::level::debug;
        case SDL_LOG_PRIORITY_INFO: return spdlog::level::info;
        case SDL_LOG_PRIORITY_WARN: return spdlog::level::warn;
        case SDL_LOG_PRIORITY_ERROR: return spdlog::level::err;
        case SDL_LOG_PRIORITY_CRITICAL: return spdlog::level::critical;
        default: return spdlog::level::info;
    }
}

void SDLCALL forwardSdlLog(void* /*userdata*/, const int category, const SDL_LogPriority priority,
                           const char* const message) {
    static const logging::Logger logger = logging::get("sdl");
    logger->log(toSpdlogLevel(priority), "[category {}] {}", category, message);
}

}  // namespace

std::string defaultLogFile() {
    std::filesystem::path directory;
#if defined(_WIN32)
    directory = std::filesystem::path(envOrEmpty("LOCALAPPDATA")) / "wheelemu" / "logs";
#elif defined(__APPLE__)
    directory = std::filesystem::path(envOrEmpty("HOME")) / "Library" / "Logs" / "wheelemu";
#else
    const std::string state = envOrEmpty("XDG_STATE_HOME");
    directory = state.empty() ? std::filesystem::path(envOrEmpty("HOME")) / ".local" / "state" / "wheelemu"
                              : std::filesystem::path(state) / "wheelemu";
#endif
    return (directory / "wheelemu-service.log").string();
}

void routeSdlLogging() {
    SDL_SetLogOutputFunction(forwardSdlLog, nullptr);
    // SDL's info level is start-up chatter; its warnings and errors are what explain device problems.
    // For more, set the SDL_LOGGING environment variable (e.g. SDL_LOGGING=input=debug).
    if (std::getenv("SDL_LOGGING") == nullptr) {
        SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);
    }
}

void logStartupBanner(const std::string& configPath) {
    const logging::Logger logger = logging::get("service");
    const int sdlVersion = SDL_GetVersion();
    WE_LOG_INFO(logger, "wheelemu-service {} starting on {} (SDL {}.{}.{}, revision {})", WHEELEMU_VERSION,
                SDL_GetPlatform(), SDL_VERSIONNUM_MAJOR(sdlVersion), SDL_VERSIONNUM_MINOR(sdlVersion),
                SDL_VERSIONNUM_MICRO(sdlVersion), SDL_GetRevision());
    WE_LOG_INFO(logger, "config: {}", configPath.empty() ? std::string("(built-in defaults)") : configPath);
    const std::string envLevels = envOrEmpty("SPDLOG_LEVEL");
    if (!envLevels.empty()) {
        WE_LOG_INFO(logger, "log levels overridden by SPDLOG_LEVEL='{}'", envLevels);
    }
}

}  // namespace wheelemu::service
