#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri {

// The shim's axis audit: what a game asks a wheel and tells it, so the log can say whether the game ever tried to
// change which axis is which (action maps, calibration, driver commands) or only read the axes where they are.
// Portable so it is unit-tested; the shim feeds it DirectInput calls (shim/src/DeviceAudit.hpp).

// Logitech's rev-light command, as the Logitech Steering Wheel SDK sends it through IDirectInputDevice8::Escape:
// command 0 and {DWORD size = 20, DWORD version = 1, float rpm, float rpm the first light comes on, float redline}.
// NFS 2015 sends one every frame (rpm 0..1, first light 0.01, redline 1). It lights LEDs; it does not touch the axes.
struct RevLights {
    float rpm = 0.0F;
    float firstLight = 0.0F;
    float redline = 0.0F;
};

// The rev lights in an Escape's input buffer, or nullopt when it is anything else.
[[nodiscard]] std::optional<RevLights> decodeRevLights(std::uint32_t command, std::span<const std::uint8_t> in);

// "14 00 00 00 01": the first `max` bytes in hex, then "... (N bytes)" when there are more.
[[nodiscard]] std::string hexBytes(std::span<const std::uint8_t> bytes, std::size_t max = 64);

// DirectInput property ids (DIPROP_* are small integers). True when setting the property can change what an axis
// reports beyond the scaling every game does: calibration, calibration points, logical or physical ranges, app data
// (action mapping), names. False for BUFFERSIZE, AXISMODE, GRANULARITY, RANGE, DEADZONE, SATURATION, FFGAIN and
// AUTOCENTER. Unknown ids count as could-remap, so nothing new slips through.
[[nodiscard]] bool setPropertyCouldRemap(std::uint32_t propertyId);

// A program module (DLL file name, any case) that can read or reconfigure a wheel without going through
// DirectInput: Logitech's SDK and software, raw HID (hid.dll), XInput, Windows.Gaming.Input, WinUSB, SDL, the old
// dinput.dll. hid.dll is also loaded by DirectInput itself, so on its own it proves nothing.
[[nodiscard]] bool isInputModule(std::string_view fileName);

// Every kind of call a game made on one device, with counts. Thread-safe: games call devices from several threads.
class AxisAudit {
public:
    enum class Risk : std::uint8_t {
        None,         // reads or scales the axes where they are
        CouldRemap,   // could change which axis is which, or what one reports
    };

    // Counts one call of `kind` (e.g. "SetProperty DIPROP_RANGE"); returns this kind's call number, 1 the first time.
    std::uint64_t record(std::string_view kind, Risk risk);

    // "1234 calls of 9 kinds; none could remap axes. Kinds: EnumObjects x2, ..." or
    // "...; COULD REMAP AXES: SetActionMap x1. Kinds: ...". Kinds in the order first seen.
    [[nodiscard]] std::string summary() const;

    [[nodiscard]] std::uint64_t couldRemapCalls() const;
    [[nodiscard]] std::uint64_t totalCalls() const;

private:
    struct Entry {
        std::string kind;
        std::uint64_t count = 0;
        Risk risk = Risk::None;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t total_ = 0;
};

}  // namespace wheelbri
