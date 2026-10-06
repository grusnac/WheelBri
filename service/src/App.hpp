#pragma once

#include "SessionUi.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace wheelbri::service {

// The command line service's --ui window: the live view of one session, closed with Esc or its close button
// (which stops the service).
// `configPath`: the config it runs with (the start of a new wheel profile in its Wheel setup tab).
[[nodiscard]] std::unique_ptr<UiHost> makeSessionWindow(std::string configPath);

// The app (wheelbri.exe, or wheelbri-service --app): one window: the state of the service, Start and Stop on every
// tab, the live view while it runs. Start runs a session on a
// thread of its own; Stop, or closing the window, ends it cleanly.
struct AppSettings {
    std::filesystem::path builtInConfigFolder;   // the configs that come with the program (config/)
    std::filesystem::path userConfigFolder;      // where the Mapping tab saves the user's own edits
    std::filesystem::path stateFile;             // remembers the way (virtual device or shim) and the rig
    std::filesystem::path logFolder;     // the Logs tab's
    std::filesystem::path devicesFolder; // the Wheel setup tab's device names: <vid>-<pid>.ini
    std::filesystem::path shimDll;       // WheelBri's dinput8.dll, put into games (target G920 in-game shim)
    std::filesystem::path shimSettings;  // config/wheelbri-shim.ini, put next to it
    // Runs one session with the config at `configPath` ("" = the built-in defaults) until it is asked to stop
    // (blocking); returns its exit code. `virtualDevice`: the virtual G920 (rig hidden) rather than the in-game shim.
    // Called on the app's session thread.
    std::function<int(UiHost& host, const std::string& configPath, bool virtualDevice)> runSession;
    // Asks the running session to stop (from the window's thread).
    std::function<void()> requestStop;
    // Clears that request before the next Start.
    std::function<void()> clearStop;
    // While stopped, with the target virtual device and every rig device connected: puts the hide filter on the rig's
    // devices that do not run it yet (what the first Start would otherwise do, ~25 s after a fresh install), saying
    // what it does through `say` when there is something to do. Returns privileged::HideSetup as an int. Blocking;
    // called on a thread of the app's own. Empty: not offered (Linux).
    std::function<int(const std::function<void(const std::string&)>& say)> prepareHide;
};

// Runs the app until its window is closed (and the session, if one runs, has stopped). Returns 0, or 1 if the
// window could not be opened. Logging must be up (the app shows its lines).
int runApp(const AppSettings& settings);

}  // namespace wheelbri::service
