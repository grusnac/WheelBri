#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Logitech's Steering Wheel SDK as games see it, for wheelbri-logi.dll (logi/): the functions a game built with the SDK
// looks up by name in the helper DLL the registry names (the ServerBinary value under HKCR\CLSID\kSdkClsid), how to
// print their arguments and results, and the SDK's constants. NFS Payback is one such game (2026-10-10: it ignores
// every wheel unless that DLL loads).
//
// Written from the SDK's public documentation (function names, arguments, constants): nothing is copied from
// Logitech's files. Portable (no Windows headers), so the formatting is unit-tested on Linux; the DLL itself reads the
// memory behind pointer arguments and hands the bytes in.
namespace wheelbri::logi {

// The CLSID whose ServerBinary value names the helper DLL.
inline constexpr std::string_view kSdkClsid = "{63BD165D-1584-4E75-AB56-08330350545F}";

// How an argument is printed. Every argument travels as a 64-bit register value (x64: the first four go in RCX, RDX,
// R8, R9; none of these functions has more), so a 32-bit int, a bool or a pointer is read from the same raw value.
enum class Arg : std::uint8_t {
    Int,            // a plain number
    Bool,
    Percent,        // a number the SDK reads as a percentage (-100..100 or 0..100)
    Pointer,        // an address (an output buffer, a reference)
    Window,         // an HWND
    Handle,         // a DirectInput device handle (the *DInput functions)
    DeviceType,     // LOGI_DEVICE_TYPE_*
    Manufacturer,   // LOGI_MANUFACTURER_*
    Model,          // LOGI_MODEL_*
    ForceType,      // LOGI_FORCE_*
    PeriodicType,   // LOGI_PERIODICTYPE_*
    Milliseconds,
    Float,          // only the LED functions, and those are typed in the DLL (the values travel in XMM registers)
};

// What the function returns.
enum class Ret : std::uint8_t {
    Void,
    Bool,
    Int,
    State,   // a pointer to the wheel's state (DIJOYSTATE2, or DIJOYSTATE2ENGINES with the same first fields)
};

// What a call is about, for its log component and level.
enum class Kind : std::uint8_t {
    Lifecycle,   // initialise, shut down: always logged
    Query,       // is it connected, which model, its name: logged when the answer changes
    Input,       // the wheel's state, buttons, the non-linear steering curve
    Force,       // force feedback
    Lights,      // the rev lights (many calls a second: sampled)
    Setup,       // operating range, controller properties: always logged
};

// Something to read besides the plain arguments and the result.
enum class Extra : std::uint8_t {
    None,
    TextBuffer,      // args[1] is a wchar_t buffer of args[2] characters, filled on success
    RangeOut,        // args[1] is an int& the range is written to
    PropertiesIn,    // args[0] points to a LogiControllerPropertiesData (passed by value: x64 hands a pointer)
    PropertiesOut,   // args[1] points to one, filled on success
    State,           // the result points to the wheel's state
};

struct Function {
    std::string_view name;
    std::uint8_t args;      // how many arguments (0..4)
    std::uint8_t keyArgs;   // the leading arguments that tell calls apart (a device index, a button)
    std::array<Arg, 4> kinds;
    std::array<std::string_view, 4> argNames;
    Ret ret;
    Kind kind;
    Extra extra;
};

// Every function games look up, in the order NFS Payback's copy of the SDK names them (48). The DLL exports exactly
// these; FunctionId is the index.
[[nodiscard]] std::span<const Function> functions();
[[nodiscard]] const Function* findFunction(std::string_view name);
[[nodiscard]] std::size_t functionIndex(std::string_view name);   // functions().size() when unknown

// "LogiPlaySpringForce(index 0, offset 0%, saturation 41%, coefficient 90%)". `args` holds at least f.args values.
[[nodiscard]] std::string formatCall(const Function& f, std::span<const std::int64_t> args);
// "true", "-1", "void"; for Ret::State the address (the DLL describes the state itself).
[[nodiscard]] std::string formatResult(const Function& f, std::int64_t raw);

// The SDK's constants by name ("G920", "spring"), or "unknown N". Device indexes are 0..3.
[[nodiscard]] std::string modelName(int model);
[[nodiscard]] std::string manufacturerName(int manufacturer);
[[nodiscard]] std::string deviceTypeName(int type);
[[nodiscard]] std::string forceTypeName(int type);
[[nodiscard]] std::string periodicTypeName(int type);

// LogiControllerPropertiesData: what a game prefers for force feedback and the wheel's range. In memory (x64, natural
// alignment): bool forceEnable @0, int overallGain @4, int springGain @8, int damperGain @12, bool defaultSpringEnabled
// @16, int defaultSpringGain @20, bool combinePedals @24, int wheelRange @28, bool gameSettingsEnabled @32,
// bool allowGameSettings @33; 36 bytes.
inline constexpr std::size_t kPropertiesSize = 36;
struct ControllerProperties {
    bool forceEnable = false;
    int overallGain = 0;
    int springGain = 0;
    int damperGain = 0;
    bool defaultSpringEnabled = false;
    int defaultSpringGain = 0;
    bool combinePedals = false;
    int wheelRange = 0;
    bool gameSettingsEnabled = false;
    bool allowGameSettings = false;
};
// nullopt when fewer than kPropertiesSize bytes.
[[nodiscard]] std::optional<ControllerProperties> decodeProperties(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string describe(const ControllerProperties& p);

// The first fields of DIJOYSTATE2 (and DIJOYSTATE2ENGINES): what the game gets from LogiGetState.
struct StateSummary {
    std::int32_t x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;
    std::array<std::int32_t, 2> sliders{};
    std::uint32_t pov0 = 0xFFFFFFFFU;
    std::uint32_t buttons = 0;   // buttons 0..31, bit n = button n
    friend bool operator==(const StateSummary&, const StateSummary&) = default;
};
// From the bytes at the state pointer: lX..lRz, rglSlider[2], rgdwPOV[0], rgbButtons[0..31] (offsets 0, 4, .., 32, 48).
// nullopt when fewer than 80 bytes.
inline constexpr std::size_t kStatePrefixSize = 80;
[[nodiscard]] std::optional<StateSummary> decodeState(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string describe(const StateSummary& s);

// Remembers the last value per function and key (its key arguments), so a call that only repeats what was said is
// not logged again. Thread-safe: games call the SDK from more than one thread.
class ChangeLog {
public:
    enum class Seen : std::uint8_t { First, Changed, Same };
    Seen observe(std::size_t function, std::int64_t key0, std::int64_t key1, std::string_view value);

private:
    std::mutex mutex_;
    std::map<std::tuple<std::size_t, std::int64_t, std::int64_t>, std::string> last_;
};

// The newest version among G HUB's wheel SDK folders ("9_1_1", "9_0_7", "10_0_2"): numbers compared part by part.
// Names that are not versions are ignored; empty when none is.
[[nodiscard]] std::string newestVersionFolder(std::span<const std::string> names);

}  // namespace wheelbri::logi
