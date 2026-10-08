#pragma once

#include "wheelbri/LogLevel.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace wheelbri::service {

// The log level the app's Logs tab picks (its Level button: warn, info, debug, trace, and back to warn), for every
// component: this process (app and session) at once; the game's shim and wheelbri-uhid through the session's
// LogLevelPacket (main.cpp); the WheelBri Helper through its Hello (HelperProbe); both drivers (virtual G920, hide
// filter) through HKLM\SOFTWARE\wheelbri LogLevel, written at each Start and, while a session runs, on a change.
// Remembered in the app's state file ([app] log_level); warn when nothing is stored. Warnings and errors are always
// written: nothing quieter than warn can be picked (LogLevel.hpp clampToApp).
//
// The command-line service (no app) never starts it: current() is then this process's level from its config.
class AppLogLevel {
public:
    using Listener = std::function<void(LogLevel level)>;

    static constexpr const char* kSettingKey = "log_level";

    static AppLogLevel& shared();

    // The app: the stored level (or warn), applied to this process. Logs nothing: at warn there is nothing to say.
    void start(const std::filesystem::path& stateFile);
    [[nodiscard]] bool started() const { return started_.load(); }

    // The app's level, or (not started) this process's own, clamped like the app's.
    [[nodiscard]] LogLevel current() const;

    // The Level button: the next level, applied to this process, remembered and handed to the listeners. Returns it.
    LogLevel next();
    // The same for one level (clamped).
    void set(LogLevel level);

    // Called on every change, on the thread that made it (the window's).
    void addListener(Listener listener);

    // How many changes so far: the session sends its packet at once when this moves.
    [[nodiscard]] std::uint64_t changes() const { return changes_.load(); }

private:
    std::atomic<bool> started_{false};
    std::atomic<std::uint8_t> level_{static_cast<std::uint8_t>(kDefaultAppLogLevel)};
    std::atomic<std::uint64_t> changes_{0};
    std::mutex mutex_;   // stateFile_, listeners_, and one change at a time
    std::filesystem::path stateFile_;
    std::vector<Listener> listeners_;
};

}  // namespace wheelbri::service
