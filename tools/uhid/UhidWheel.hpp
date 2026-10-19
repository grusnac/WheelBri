#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/HidPid.hpp"
#include "wheelbri/HidPp.hpp"

#include <linux/uhid.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <string>
#include <string_view>

namespace wheelbri::uhid {

// The virtual G920 or G25 on Linux (Phase 4), through the kernel's uhid: the same HID device the Windows driver
// presents (19 buttons with the shifter's, hat, HID PID force feedback), so a game reading it raw sees what it sees on
// Windows. The G920's has clutch and throttle named for Proton's G920 hack (g920ProtonReportDescriptor); the G25's is
// the Windows descriptor as it is (Proton has no hack for it). Proton reads it raw only when asked
// (PROTON_ENABLE_HIDRAW=0x046D/0xC262, or 0xC299 for the G25); its dinput then speaks HID PID to it, which HidPid turns
// into the service's FfbPackets.
//
// The G920 is a USB device to the kernel, as a real one: hid-logitech-hidpp takes it, finds no HID++ and leaves it to
// the generic driver. The G25 is a virtual-bus device instead: the kernel's hid-lg claims USB 046D:C299 and refuses
// any device that is not really on USB (so nothing would read it); on the virtual bus hid-generic takes it.
//
// No file descriptors here: `write` sends one uhid_event to the kernel and handle() takes the ones it sends
// back, so the protocol is unit-tested without /dev/uhid.
class UhidWheel {
public:
    using Write = std::function<bool(const uhid_event&)>;
    using Log = HidPid::Logger;   // a line and its level

    // `wheel`: which one (virtualWheel("g920"), virtualWheel("g25")); static, so it outlives this.
    UhidWheel(Write write, HidPid::Sender ffb, Log log, const VirtualWheel& wheel = *virtualWheel("g920"));

    // "Logitech G25 Racing Wheel USB": its device name, as the shim reports it.
    [[nodiscard]] std::string_view name() const { return wheel_.oemName; }
    [[nodiscard]] const VirtualWheel& wheel() const { return wheel_; }
    // "G920", "G25": for the log.
    [[nodiscard]] std::string label() const { return std::string(wheel_.instancePrefix); }

    // UHID_CREATE2: the device appears (hidraw + evdev nodes). False if the kernel refused it.
    bool create();
    // UHID_DESTROY: it goes away (also when the process exits: closing /dev/uhid destroys it).
    void destroy();

    // The wheel's state as an input report. Sent when it changed, or when `force` (keep-alive).
    // Whether it was written (false: unchanged and not forced).
    bool submit(const TargetReport& report, bool force);

    // One event read from /dev/uhid.
    void handle(const uhid_event& event, std::int64_t nowMs);

    // Every ~10 ms: effects that ran out, keep-alives to the service.
    void tick(std::int64_t nowMs) { pid_.tick(nowMs); }

    // Everything stops (going away).
    void stopForces(std::int64_t nowMs) { pid_.stopAll(nowMs); }

    [[nodiscard]] bool started() const { return started_; }
    [[nodiscard]] int openCount() const { return openCount_; }     // programs reading it (the game)
    [[nodiscard]] std::uint64_t reportsHandled() const { return pid_.reportsHandled(); }
    [[nodiscard]] std::uint64_t packetsSent() const { return pid_.packetsSent(); }
    [[nodiscard]] std::size_t activeSlots() const { return pid_.activeSlots(); }

private:
    void reply(std::uint32_t type, std::uint32_t id, std::uint16_t err, const std::uint8_t* data, std::size_t size);
    // A HID++ request (VirtualWheel::hidpp): its answer goes back as an input report.
    void hidppRequest(std::span<const std::uint8_t> report, std::int64_t nowMs);

    Write write_;
    Log log_;
    const VirtualWheel& wheel_;
    HidPid pid_;
    std::unique_ptr<HidPp> hidpp_;   // for a wheel with HID++ only
    bool started_ = false;
    int openCount_ = 0;
    bool haveLast_ = false;
    std::set<std::pair<std::uint32_t, std::uint32_t>> unsupported_;   // get-report kinds already logged
    InputReport last_{};
};

}  // namespace wheelbri::uhid
