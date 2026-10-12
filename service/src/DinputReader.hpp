#pragma once

#include "wheelbri/WheelState.hpp"

#include <memory>
#include <string>

namespace wheelbri::service {

// Reads a wheel through DirectInput (Windows only): the device state directly, in microseconds, never
// re-scanning devices (SDL's joystick update did, for 130-300 ms whenever any device came or went).
//
// The axes, buttons and hat are numbered exactly as SDL3's DirectInput back-end numbers them (axes sorted by
// their DIJOYSTATE2 field: X, Y, Z, Rx, Ry, Rz, sliders; buttons and POVs in DirectInput's order), so the
// [mapping] indices in the config, and the --ui window, stay what they were when SDL read the wheel.
class DinputReader {
public:
    // Opens the DirectInput device whose HID path is `hidPath`. Null + `error` if not.
    [[nodiscard]] static std::unique_ptr<DinputReader> open(const std::string& hidPath, std::string& error);
    ~DinputReader();

    DinputReader(const DinputReader&) = delete;
    DinputReader& operator=(const DinputReader&) = delete;

    [[nodiscard]] int axes() const;
    [[nodiscard]] int buttons() const;
    [[nodiscard]] int hats() const;
    // An auto-reset event DirectInput sets whenever the device's state changes (SetEventNotification), for the
    // service loop; null when the device is polled (DIDC_POLLEDDEVICE) or Windows refused it.
    [[nodiscard]] void* notification() const;

    // Latest values into `state` (axes -1..1 like SDL's /32767, buttons bit n, hat 0..7 or centred).
    // False once the device is gone (unplugged): half a second of failed reads (by the clock, so also when the events
    // loop reads only a few times a second).
    bool read(RawInputState& state);

private:
    struct Impl;
    explicit DinputReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelbri::service
