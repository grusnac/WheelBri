#include "ServiceLogging.hpp"

#include "Platform.hpp"

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

void logStartupBanner(const std::string& configPath) {
    const logging::Logger logger = logging::get("service");
    WE_LOG_INFO(logger, "wheelemu-service {} starting on {}", WHEELEMU_VERSION, platform::name());
    WE_LOG_INFO(logger, "config: {}", configPath.empty() ? std::string("(built-in defaults)") : configPath);
    const std::string envLevels = envOrEmpty("SPDLOG_LEVEL");
    if (!envLevels.empty()) {
        WE_LOG_INFO(logger, "log levels overridden by SPDLOG_LEVEL='{}'", envLevels);
    }
}

}  // namespace wheelemu::service
