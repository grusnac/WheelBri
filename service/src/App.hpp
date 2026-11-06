// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SessionUi.hpp"

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/ShimGames.hpp"

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
    ShimSources shimDlls;                // WheelBri's dinput8.dll (64- and 32-bit), put into games (in-game shim)
    std::filesystem::path shimSettings;  // config/wheelbri-shim.ini, put next to it
    // Runs one session with the config at `configPath` ("" = the built-in defaults) until it is asked to stop
    // (blocking); returns its exit code. `virtualDevice`: the virtual wheel (rig hidden) rather than the in-game shim.
    // `order`: the Mapping tab's button order (ConfigPicker::buttonOrder), used instead of the file's [buttons] order.
    // `trueForce`: the Mapping tab's TrueForce (ConfigPicker::trueForceAtStart), used instead of the file's
    // [emulator] trueforce. Called on the app's session thread.
    std::function<int(UiHost& host, const std::string& configPath, bool virtualDevice, ButtonOrder order,
                      bool trueForce)>
        runSession;
    // Where Logitech's TrueForce SDK is installed ("" = not): the Mapping tab's TrueForce switch is greyed without it.
    std::function<std::string()> trueForceSdk;
    // Asks the running session to stop (from the window's thread).
    std::function<void()> requestStop;
    // Clears that request before the next Start.
    std::function<void()> clearStop;
};

// Runs the app until its window is closed (and the session, if one runs, has stopped). Returns 0, or 1 if the
// window could not be opened. Logging must be up (the app shows its lines).
int runApp(const AppSettings& settings);

}  // namespace wheelbri::service
