#pragma once

#include <functional>
#include <memory>
#include <string>

namespace wheelbri::service {

// The virtual Xbox 360 controller next to the virtual G920, created while the service runs (--virtual-g920, unless
// --xbox-pad off) and removed when it stops or crashes. Two Windows software devices, as a real wired one has two
// sides: wheelbri\XUSB (driver/xusb) for XInput, which games use for their Xbox prompts, and wheelbri\XBOXHID (the
// wheelbri-g920 driver) for HID / DirectInput, which joy.cpl lists. The service feeds both over UDP
// (core/XboxPad.hpp, core/HidXbox360.hpp).
class VirtualXboxPad {
public:
    // Null + `error` if it cannot be created (driver not installed, not Administrator, not Windows).
    // `cancelled`: checked while waiting for Windows (Stop pressed): then null, "cancelled".
    [[nodiscard]] static std::unique_ptr<VirtualXboxPad> create(std::string& error, const std::function<bool()>& cancelled = {});
    ~VirtualXboxPad();

    VirtualXboxPad(const VirtualXboxPad&) = delete;
    VirtualXboxPad& operator=(const VirtualXboxPad&) = delete;

    [[nodiscard]] const std::string& instanceId() const;
    // The HID side is there too (joy.cpl lists the controller).
    [[nodiscard]] bool hidSide() const;

private:
    struct Impl;
    explicit VirtualXboxPad(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelbri::service
