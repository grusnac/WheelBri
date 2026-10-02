#pragma once

#include <functional>
#include <memory>
#include <string>

namespace wheelemu::service {

// The virtual G920 served by our own driver (driver/, Phase 5 plan B), created while the service runs
// (--virtual-g920) and removed when it stops, including a crash: it is a Windows "software device" tied
// to this process.
//
// On Windows it also keeps DirectInput's registry entries for VID_046D&PID_C262 right: the name and the
// "driving" device type the shim reports, and no foreign force feedback driver (Logitech's software
// registers one for every G920 that only works with a real one; checked every 2 s, as the bridge did).
//
// If a program still held an earlier G920 when it was removed, Windows may not give a new one at the same
// identity a driver until it restarts; then a spare identity is used, as the bridge does.
class VirtualG920 {
public:
    // Null + `error` if it cannot be created (not Windows, driver not installed, not Administrator, ...).
    // `cancelled`: checked while waiting for Windows (Stop pressed): then null, "cancelled".
    [[nodiscard]] static std::unique_ptr<VirtualG920> create(std::string& error, const std::function<bool()>& cancelled = {});
    ~VirtualG920();

    VirtualG920(const VirtualG920&) = delete;
    VirtualG920& operator=(const VirtualG920&) = delete;

    [[nodiscard]] const std::string& instanceId() const;

private:
    struct Impl;
    explicit VirtualG920(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelemu::service
