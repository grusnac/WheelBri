#pragma once

#include "wheelbri/Devices.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The physical wheel: finding it, reading it and playing force feedback on it, with each system's own API:
//   Windows: DirectInput (WheelDinput.cpp)
//   Linux:   evdev, /dev/input/event* (WheelEvdev.cpp)
// Both number the axes, buttons and hats the way the service always has (see DinputReader.hpp and
// EvdevLayout.hpp), so the config stays valid.
namespace wheelbri::service {

struct WheelInfo {
    std::string name;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string path;   // HID path (Windows), /dev/input/eventN (Linux)
};

class Wheel : public IInputSource {
public:
    [[nodiscard]] virtual const WheelInfo& info() const = 0;
    [[nodiscard]] std::string name() const override { return info().name; }
    [[nodiscard]] virtual int axes() const = 0;
    [[nodiscard]] virtual int buttons() const = 0;
    [[nodiscard]] virtual int hats() const = 0;

    // Force feedback on this wheel; nullptr (and the reason in the log) if it has none or it cannot be used.
    // Effects should already have passed the FfbSafetyLimiter. `invert` mirrors every effect.
    [[nodiscard]] virtual std::unique_ptr<IFfbSink> openForceFeedback(bool invert) = 0;
};

class WheelBackend {
public:
    virtual ~WheelBackend() = default;

    // The game controllers connected now.
    [[nodiscard]] virtual std::vector<WheelInfo> list() = 0;
    // Opens one of them; nullptr (and the reason in the log) if it cannot be read.
    [[nodiscard]] virtual std::unique_ptr<Wheel> open(const WheelInfo& wheel) = 0;
    // For the log: "DirectInput", "evdev".
    [[nodiscard]] virtual std::string describe() const = 0;
};

// The backend for this system. Nullptr (and the reason in the log) if it cannot start.
[[nodiscard]] std::unique_ptr<WheelBackend> startWheelBackend();

// The first connected wheel whose name or vid:pid contains `match` ([emulator] source_device); nullptr if
// none (the log lists what is connected).
// `quiet`: a retry (waiting for a lost wheel), which logs less and a failure every 30 s at most.
[[nodiscard]] std::unique_ptr<Wheel> openWheel(WheelBackend& backend, const std::string& match, bool quiet = false);

// Whether a wheel matching `match` is connected now. Logs nothing: for polling while waiting for one.
[[nodiscard]] bool wheelConnected(WheelBackend& backend, const std::string& match);

}  // namespace wheelbri::service
