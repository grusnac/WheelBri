// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/DeviceHolders.hpp"

#include <string>
#include <vector>

namespace wheelbri::service {

// The programs, other than this one, that have the device `instanceId` or any device below it open now: what keeps
// Windows from restarting the wheel with the hide filter ("a program has it open", PNP_VetoOutstandingOpen, says only
// the device). Read from the computer's handle list, as Sysinternals Handle does: every file handle of every other
// process whose object is one of those devices. Needs Administrator (the service with --virtual-wheel, or the WheelBri
// Helper); without it, or when Windows will not say, it finds fewer or none. Takes up to a few seconds: only for the
// message after Windows refused, never on a regular path. Windows only; empty elsewhere.
[[nodiscard]] std::vector<holders::Holder> deviceHolders(const std::string& instanceId);

}  // namespace wheelbri::service
