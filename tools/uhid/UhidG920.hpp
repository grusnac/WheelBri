#pragma once

#include "wheelbri/DeviceProfile.hpp"
#include "wheelbri/HidG920.hpp"
#include "wheelbri/HidPid.hpp"

#include <linux/uhid.h>

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <utility>
#include <string>

namespace wheelbri::uhid {

// The virtual G920 on Linux (Phase 4), through the kernel's uhid: the same HID device the Windows bridge
// creates with HIDMaestro (HidG920.hpp: wheel X, brake Y, clutch Rz, throttle Slider, 19 buttons with the shifter's, hat, HID PID
// force feedback), so a game reading it raw sees what NFS 2015 sees on Windows. Proton reads it raw only when
// asked (PROTON_ENABLE_HIDRAW=0x046D/0xC262); its dinput then speaks HID PID to it, which HidPid turns into the
// service's FfbPackets.
//
// No file descriptors here: `write` sends one uhid_event to the kernel and handle() takes the ones it sends
// back, so the protocol is unit-tested without /dev/uhid.
class UhidG920 {
public:
    using Write = std::function<bool(const uhid_event&)>;
    using Log = HidPid::Logger;   // a line and its level

    static constexpr const char* kName = "Logitech G920 Driving Force Racing Wheel USB";   // as the shim reports

    UhidG920(Write write, HidPid::Sender ffb, Log log);

    // UHID_CREATE2: the device appears (hidraw + evdev nodes). False if the kernel refused it.
    bool create();
    // UHID_DESTROY: it goes away (also when the process exits: closing /dev/uhid destroys it).
    void destroy();

    // The wheel's state as an input report. Sent when it changed, or when `force` (keep-alive).
    void submit(const TargetReport& report, bool force);

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

    Write write_;
    Log log_;
    HidPid pid_;
    bool started_ = false;
    int openCount_ = 0;
    bool haveLast_ = false;
    std::set<std::pair<std::uint32_t, std::uint32_t>> unsupported_;   // get-report kinds already logged
    std::array<std::uint8_t, kG920InputReportSize> last_{};
};

}  // namespace wheelbri::uhid
