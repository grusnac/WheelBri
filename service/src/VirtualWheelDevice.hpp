// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace wheelbri {
struct VirtualWheel;
}

namespace wheelbri::service {

// The virtual wheel served by our own driver (driver/): a G25, G27, G920 or G923 (VirtualWheel,
// HidWheelDevice.hpp), created while the service runs (--virtual-wheel) and removed when it stops, including a crash:
// it is a Windows "software device" tied to this process.
//
// On Windows it also keeps DirectInput's registry entries for its ids (VID_046D&PID_C262, PID_C299) right: the name and
// the "driving" device type the shim reports, and no foreign force feedback driver (Logitech's software registers one
// for every G920 or G25 that only works with a real one; checked every 2 s, as the bridge did).
//
// If a program still held an earlier one when it was removed, Windows may not give a new one at the same identity a
// driver until it restarts; then a spare identity is used, as the bridge does.
class VirtualWheelDevice {
public:
    // Whether Windows has our driver package (wheelbri-wheel.inf in its driver store). Unknown when that cannot be
    // read (or not Windows): then create() just tries.
    enum class DriverState { Installed, Missing, Unknown };
    [[nodiscard]] static DriverState driverState();

    // Null + `error` if it cannot be created (not Windows, driver not installed, not Administrator, ...).
    // `cancelled`: checked while waiting for Windows (Stop pressed): then null, "cancelled".
    // `userHive`: also the user's DirectInput entries (HKCU); false in the WheelBri Helper, where HKCU is SYSTEM's.
    // `wheel`: which one (virtualWheel("g920"), virtualWheel("g25")); it must outlive this (the table is static).
    // `trueForce`: for a wheel that has it (VirtualWheel::trueForce), also its TrueForce interface, a second software
    // device in the wheel's container (TrueForce.hpp); if that fails the wheel is still made, the log says why.
    // DirectInput's force feedback driver entry for the wheel's model (LogitechFfbDriver.hpp): for the G920 and G923
    // Logitech's stays when its software wrote one, for the G25 and G27 any other program's goes; checked every 2 s.
    [[nodiscard]] static std::unique_ptr<VirtualWheelDevice> create(const VirtualWheel& wheel, std::string& error,
                                                             const std::function<bool()>& cancelled = {},
                                                             bool userHive = true,
                                                             bool trueForce = false);
    ~VirtualWheelDevice();

    VirtualWheelDevice(const VirtualWheelDevice&) = delete;
    VirtualWheelDevice& operator=(const VirtualWheelDevice&) = delete;

    [[nodiscard]] const std::string& instanceId() const;

private:
    struct Impl;
    explicit VirtualWheelDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Logitech's TrueForce SDK as G HUB installs it (Program Files\Logi\trueforce\<version>\trueforce_sdk_x64.dll, the
// highest version there): a game that uses TrueForce (ACC) loads it to stream its force to the virtual G923's
// TrueForce interface. Empty when it is not there (G HUB not installed), and anywhere but Windows.
[[nodiscard]] std::string trueForceSdkPath();

// Whether Logitech's older-wheel force feedback driver for the wheel's model is installed (G HUB's
// jerry_forcefeedback_x64.dll, registered with a class per model: LogitechFfbDriver.hpp's classicClsid) with its DLL
// there. Only the G25 and G27 have one; false anywhere but Windows.
[[nodiscard]] bool classicFfbDriverInstalled(const VirtualWheel& wheel);

// The user's own DirectInput entries for the virtual wheel (HKCU: its name, the "driving" type, no foreign force
// feedback driver but Logitech's for the G920 and G923, checked every 2 s), for an app whose wheel the WheelBri Helper creates: the helper, as SYSTEM,
// keeps only the machine's (HKLM). Written while this exists, removed when it goes. Nothing elsewhere than Windows.
class DirectInputUserEntries {
public:
    explicit DirectInputUserEntries(const VirtualWheel& wheel);
    ~DirectInputUserEntries();
    DirectInputUserEntries(const DirectInputUserEntries&) = delete;
    DirectInputUserEntries& operator=(const DirectInputUserEntries&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelbri::service
