// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wheelbri::holders {

// Which programs have a device open, for the message when Windows will not restart the wheel: the hide filter starts
// only when the wheel restarts, and any program with it open refuses that (2026-11-02: Firefox, whose gamepad support
// opens every game controller, kept Start failing with only "a program has it open"). The service lists every open
// handle on the computer and keeps those whose object is one of the wheel's device objects (service/src/
// DeviceHolders.cpp, Windows only); what can be decided without Windows is here, portable and unit-tested.

struct Holder {
    std::uint32_t pid = 0;
    std::string program;   // "firefox.exe"; empty when Windows would not say
};

// Whether a handle's object name ("\Device\000000e3", as NtQueryObject gives it) is one of `deviceNames` (the device
// objects of the wheel's interfaces, DEVPKEY_Device_PDOName), ignoring case; also a name below one
// ("\Device\000000e3\x": a device opened with a path after its name). An empty name never matches.
[[nodiscard]] bool namesDevice(std::string_view handleName, const std::vector<std::string>& deviceNames);

// "firefox.exe (pid 14196), XboxService.exe (pid 7356)": each process once, in pid order; one without a name as
// "pid 4242". Empty for none.
[[nodiscard]] std::string describe(std::vector<Holder> holders);

}  // namespace wheelbri::holders
