#pragma once

#include <string_view>

namespace wheelbri::shim {

// Logs the game's loaded modules (DLLs), so the log shows whether anything besides DirectInput could read or
// reconfigure the wheel: Logitech's SDK or software, raw HID, XInput, Windows.Gaming.Input (wheelbri::isInputModule).
// The first call lists them all (input-related ones at info, the rest at debug); later calls log only modules loaded
// since, at info. `when` says where in the game's start it was taken ("DirectInput8Create", "CreateDevice", ...).
// Takes a Toolhelp snapshot (kernel32 only, a few milliseconds): call at start-up and lifecycle points, never per
// frame, and never from DllMain.
void logNewModules(std::string_view when);

}  // namespace wheelbri::shim
