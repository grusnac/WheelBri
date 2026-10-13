#pragma once

#include <functional>
#include <memory>
#include <string>

namespace wheelbri {
struct VirtualWheel;
}

namespace wheelbri::service {

// The virtual wheel served by our own driver (driver/, Phase 5 plan B): a G920 or a G25 (VirtualWheel, HidG920.hpp),
// created while the service runs (--virtual-g920) and removed when it stops, including a crash: it is a Windows
// "software device" tied to this process.
//
// On Windows it also keeps DirectInput's registry entries for its ids (VID_046D&PID_C262, PID_C299) right: the name and
// the "driving" device type the shim reports, and no foreign force feedback driver (Logitech's software registers one
// for every G920 or G25 that only works with a real one; checked every 2 s, as the bridge did).
//
// If a program still held an earlier one when it was removed, Windows may not give a new one at the same identity a
// driver until it restarts; then a spare identity is used, as the bridge does.
class VirtualG920 {
public:
    // Whether Windows has our driver package (wheelbri-g920.inf in its driver store). Unknown when that cannot be
    // read (or not Windows): then create() just tries.
    enum class DriverState { Installed, Missing, Unknown };
    [[nodiscard]] static DriverState driverState();

    // Null + `error` if it cannot be created (not Windows, driver not installed, not Administrator, ...).
    // `cancelled`: checked while waiting for Windows (Stop pressed): then null, "cancelled".
    // `userHive`: also the user's DirectInput entries (HKCU); false in the WheelBri Helper, where HKCU is SYSTEM's.
    // `wheel`: which one (virtualWheel("g920"), virtualWheel("g25")); it must outlive this (the table is static).
    [[nodiscard]] static std::unique_ptr<VirtualG920> create(const VirtualWheel& wheel, std::string& error,
                                                             const std::function<bool()>& cancelled = {},
                                                             bool userHive = true);
    ~VirtualG920();

    VirtualG920(const VirtualG920&) = delete;
    VirtualG920& operator=(const VirtualG920&) = delete;

    [[nodiscard]] const std::string& instanceId() const;

private:
    struct Impl;
    explicit VirtualG920(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The user's own DirectInput entries for the virtual wheel (HKCU: its name, the "driving" type, no foreign force
// feedback driver, checked every 2 s), for an app whose wheel the WheelBri Helper creates: the helper, as SYSTEM,
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
