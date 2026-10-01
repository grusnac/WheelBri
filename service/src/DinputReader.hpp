#pragma once

#include "wheelemu/WheelState.hpp"

#include <memory>
#include <string>

namespace wheelemu::service {

// Reads a wheel straight through DirectInput (Windows only), bypassing SDL's joystick update.
//
// Why: SDL reads its DirectInput joysticks inside SDL_UpdateJoysticks, and that same call re-scans every
// game controller whenever any device is plugged in or removed (the bridge's virtual G920, a USB stick):
// 130-300 ms without fresh wheel input, measured. Reading the device state directly takes microseconds and
// never scans. SDL keeps the wheel open for force feedback, which does not need its joystick update.
//
// The axes, buttons and hat are numbered exactly as SDL's DirectInput back-end numbers them (axes sorted by
// their DIJOYSTATE2 field: X, Y, Z, Rx, Ry, Rz, sliders; buttons and POVs in DirectInput's order), so the
// [mapping] indices in the config, and the --ui window, stay the same.
class DinputReader {
public:
    // Opens the DirectInput device whose HID path is `hidPath` (SDL_GetJoystickPath). Null + `error` if not.
    [[nodiscard]] static std::unique_ptr<DinputReader> open(const std::string& hidPath, std::string& error);
    ~DinputReader();

    DinputReader(const DinputReader&) = delete;
    DinputReader& operator=(const DinputReader&) = delete;

    [[nodiscard]] int axes() const;
    [[nodiscard]] int buttons() const;
    [[nodiscard]] int hats() const;

    // Latest values into `state` (axes -1..1 like SDL's /32767, buttons bit n, hat 0..7 or centred).
    // False once the device is gone (unplugged): about half a second of failed reads in a row.
    bool read(RawInputState& state);

private:
    struct Impl;
    explicit DinputReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelemu::service
