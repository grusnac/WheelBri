#pragma once

#include "MappingView.hpp"

#include "wheelemu/DeviceProfile.hpp"
#include "wheelemu/FfbController.hpp"
#include "wheelemu/MapperConfig.hpp"
#include "wheelemu/WheelState.hpp"

#include <optional>
#include <string>

// Where a running session (the wheel read, mapped and published at 1 kHz) shows itself: the `--ui` window of
// the command line service, or the app's window (App.cpp).
namespace wheelemu::service {

// What does not change while a session runs.
struct SessionInfo {
    std::string sourceName;     // the physical wheel
    int axes = 0;
    int buttons = 0;
    std::string targetProfile;  // "g920", "g29"
    MapperConfig mapper;
    FilesDisplay files;
    std::string ffbStatus;      // why there is no game force feedback, when there is none
    bool ffbInvert = false;
    // The virtual devices with --virtual-g920 (the app): "virtual G920 on, R3 hidden (wheelemu-hide), Xbox pad on";
    // empty without them. `devicesWarning`: something the game needs is missing (e.g. the R3 is not hidden).
    std::string devices;
    bool devicesWarning = false;
};

// What the window draws, copied from the loop's thread about 60 times a second.
struct UiFrame {
    bool ready = false;                           // false until the loop produced its first frame
    RawInputState raw;
    WheelState state;
    TargetReport report;
    std::optional<FfbController> ffb;             // copy-constructed each time (it cannot be assigned)
    TelemetryFiles telemetry;
    std::string oldFiles;                         // Housekeeping::status()
    bool oldFilesFailed = false;
    std::uint64_t wheelLostMs = 0;                // > 0: the wheel is gone, waiting this long for it to come back
};

class UiHost {
public:
    virtual ~UiHost() = default;

    // The session is up: the wheel is open and the devices created. False if the host cannot show it (the
    // session then ends). Called on the thread that runs the session.
    virtual bool sessionStarted(const SessionInfo& info) = 0;

    // About 60 times a second, on the same thread. False when the user asked to stop (the window was closed).
    virtual bool frame(const UiFrame& frame) = 0;
};

}  // namespace wheelemu::service
