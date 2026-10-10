#pragma once

#include <cstddef>

namespace wheelbri::logi {

// Logitech's own wheel SDK helper DLL, which wheelbri-logi.dll passes every call on to. Found on the first call (never
// from DllMain), in this order:
//   1. wheelbri-logi.ini next to this DLL: [logi] real_library = <full path>;
//   2. G HUB's: %ProgramW6432%\Logi\wheel_sdk\<newest version>\logi_steering_wheel_x64.dll;
//   3. the old Logitech Gaming Software's: %ProgramW6432%\Logitech\Gaming Software\SDKs\LogitechSteeringWheel.dll.
// A candidate that is this DLL itself is skipped (the registry may point every name at us). When none loads, every
// call answers 0 (false, no state): the game sees no wheel, as with no helper DLL at all, and the log says why.
//
// Returns the address of function `index` (LogiSdk.hpp functions()) in the real DLL, or nullptr when the DLL or that
// function is missing. Thread-safe.
[[nodiscard]] void* realFunction(std::size_t index);

}  // namespace wheelbri::logi
