// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/AxisAudit.hpp"

#include <windows.h>
#include <dinput.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace wheelbri::shim {

// The axis audit of one device the game opened (the emulated wheel or a passed-through one), logged as "shim.audit".
// Every call that describes or configures the device is recorded (wheelbri::AxisAudit); the first call of each kind is
// logged at info with its details (a call that could remap axes at warn, the first 20 of them), and a summary says
// what the game did and whether any of it could have changed which axis is which:
//   [G920 ...]: axis audit at Release: 5123 calls of 11 kinds; none could remap axes. Kinds: EnumObjects flags=0x3 x1, ...
// Summaries: at Unacquire and Release, after the first minute and then every 5 minutes of reading the device (a game
// may never release it before it exits).
class DeviceAudit {
public:
    explicit DeviceAudit(std::string label) : label_(std::move(label)) {}

    // A call with no risk to the axes: `kind` is counted, `details` logged the first time.
    void call(std::string_view kind, std::string_view details = {});
    // A call that could remap axes (action maps, SendDeviceData, the control panel): logged at warn.
    void risky(std::string_view kind, std::string_view details = {});

    void getProperty(REFGUID property, LPCDIPROPHEADER header, HRESULT result);
    void setProperty(REFGUID property, LPCDIPROPHEADER header, HRESULT result);
    // After the call, so the output buffer holds the device's answer.
    void escape(LPDIEFFESCAPE escape, HRESULT result);

    // From GetDeviceState / GetDeviceData: cheap (one clock read); logs the periodic summaries.
    void tick();
    // Logs the summary now and looks for newly loaded modules.
    void summary(std::string_view when);

private:
    void record(std::string_view kind, std::string_view details, AxisAudit::Risk risk);

    std::string label_;
    AxisAudit audit_;
    std::atomic<std::int64_t> nextSummaryMs_{-1};   // steady clock; -1: not reading yet
    std::atomic<int> periodicSummaries_{0};
};

}  // namespace wheelbri::shim
