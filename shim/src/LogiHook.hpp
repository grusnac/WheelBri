#pragma once

namespace wheelbri::shim {

// [shim] logi_log = true: a debugging aid for games that use Logitech's SDKs (ACC in TrueForce mode reads the wheel
// through trueforce_sdk_x64.dll, not through DirectInput). The game checks that DLL's signature, so it cannot be
// replaced; instead the game's own import of GetProcAddress is patched, and every logi* function it looks up in a
// Logitech SDK DLL (trueforce_sdk_*, logi_steering_wheel_*) is handed back wrapped: the wrapper calls Logitech's
// function unchanged and logs its arguments (RCX..R9, XMM0..3), its result (RAX, XMM0) and the 8 bytes each pointer
// argument points to after the call. The first kFullCalls calls of each function in full, then one a second.
// x64 MinGW builds only. The import is patched from DllMain (armLogiHook: the game may load and look up Logitech's
// DLL before its first DirectInput8Create, as ACC does); without logi_log the hook only passes every lookup on.
// installLogiHook, at the first DirectInput8Create, says in the log whether it is on.
void armLogiHook() noexcept;
void installLogiHook();

inline constexpr int kFullCalls = 30;

}  // namespace wheelbri::shim
