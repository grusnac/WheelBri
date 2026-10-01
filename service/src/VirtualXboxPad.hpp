#pragma once

#include <memory>
#include <string>

namespace wheelemu::service {

// The virtual Xbox 360 controller (driver/xusb) next to the virtual G920: a Windows software device with
// hardware ID wheelemu\XUSB, created while the service runs (--virtual-g920, unless --xbox-pad off) and removed
// when it stops or crashes. The service feeds it over UDP (core/XboxPad.hpp).
class VirtualXboxPad {
public:
    // Null + `error` if it cannot be created (driver not installed, not Administrator, not Windows).
    [[nodiscard]] static std::unique_ptr<VirtualXboxPad> create(std::string& error);
    ~VirtualXboxPad();

    VirtualXboxPad(const VirtualXboxPad&) = delete;
    VirtualXboxPad& operator=(const VirtualXboxPad&) = delete;

    [[nodiscard]] const std::string& instanceId() const;

private:
    struct Impl;
    explicit VirtualXboxPad(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelemu::service
