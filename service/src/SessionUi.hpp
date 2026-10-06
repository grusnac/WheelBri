#pragma once

#include "MappingView.hpp"

#include "wheelbri/DeviceLabels.hpp"
#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/FfbController.hpp"
#include "wheelbri/MapperConfig.hpp"
#include "wheelbri/WheelState.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

// Where a running session (the wheel read, mapped and published at 1 kHz) shows itself: the `--ui` window of
// the command line service, or the app's window (App.cpp).
namespace wheelbri::service {

// What does not change while a session runs.
struct SessionInfo {
    std::string sourceName;     // the physical wheel
    std::uint16_t vendorId = 0; // its USB ids (the Wheel setup tab finds its profile by them)
    std::uint16_t productId = 0;
    int axes = 0;
    int buttons = 0;
    std::string targetProfile;  // "g920"
    MapperConfig mapper;
    // The accessories the mapping reads (device 1, 2, 3; empty ids = none), named from the Wheel setup tab.
    std::array<DeviceLabels, MapperConfig::kMaxAccessories> accessories{};
    FilesDisplay files;
    std::string ffbStatus;      // why there is no game force feedback, when there is none
    bool ffbInvert = false;
    // The virtual devices with --virtual-g920 (the app): "virtual G920 on, rig hidden (1 of 1 devices)";
    // empty without them. `devicesWarning`: something the game needs is missing (e.g. a rig device is not hidden).
    std::string devices;
    bool devicesWarning = false;
};

// What the window draws, copied from the loop's thread about 60 times a second.
struct UiFrame {
    bool ready = false;                           // false until the loop produced its first frame
    RawInputState raw;                            // the wheel's (inputs.devices[0])
    RawInputs inputs;                             // every device's
    WheelState state;
    TargetReport report;
    std::optional<FfbController> ffb;             // copy-constructed each time (it cannot be assigned)
    TelemetryFiles telemetry;
    std::string oldFiles;                         // Housekeeping::status()
    bool oldFilesFailed = false;
    std::uint64_t wheelLostMs = 0;                // > 0: the wheel is gone, waiting this long for it to come back
    bool gameIgnoresWheel = false;                // pedals pressed, but the game never used the emulated wheel
};

// UiHost::step while the hide filter is put on the rig for the first time (once per installation, ~10-40 s): the app
// shows a popup that says so.
inline constexpr const char* kStepFirstHide = "hiding the wheel, first time: up to a minute";

class UiHost {
public:
    virtual ~UiHost() = default;

    // The session is up: the wheel is open and the devices created. False if the host cannot show it (the
    // session then ends). Called on the thread that runs the session.
    virtual bool sessionStarted(const SessionInfo& info) = 0;

    // About 30 times a second, on the same thread. False when the user asked to stop (the window was closed).
    virtual bool frame(const UiFrame& frame) = 0;

    // While the session starts: what it is doing now when that can take a while ("hiding the wheel for the first
    // time"); empty when that is over. The app shows it next to Start. Called on the session's thread.
    virtual void step(const std::string& what) { (void)what; }
};

}  // namespace wheelbri::service
