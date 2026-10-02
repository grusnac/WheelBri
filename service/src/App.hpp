#pragma once

#include "SessionUi.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace wheelemu::service {

// The command line service's --ui window: the live view of one session, closed with Esc or its close button
// (which stops the service).
[[nodiscard]] std::unique_ptr<UiHost> makeSessionWindow();

// The app (wheelemu.exe, or wheelemu-service --app): one window with Start, Stop and Open logs buttons, the
// state of the service, the live view while it runs and the latest log lines. Start runs a session on a
// thread of its own; Stop, or closing the window, ends it cleanly.
struct AppSettings {
    std::string configPath;              // shown in the window
    std::string mode;                    // what Start does, in words ("virtual G920 + Xbox controller, R3 hidden")
    std::filesystem::path logFolder;     // Open logs
    // Runs one session until it is asked to stop (blocking); returns its exit code. Called on the app's
    // session thread.
    std::function<int(UiHost& host)> runSession;
    // Asks the running session to stop (from the window's thread).
    std::function<void()> requestStop;
    // Clears that request before the next Start.
    std::function<void()> clearStop;
};

// Runs the app until its window is closed (and the session, if one runs, has stopped). Returns 0, or 1 if the
// window could not be opened. Logging must be up (the app shows its lines).
int runApp(const AppSettings& settings);

}  // namespace wheelemu::service
