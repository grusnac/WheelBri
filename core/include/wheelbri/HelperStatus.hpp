// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/HelperProtocol.hpp"

#include <optional>
#include <string>

namespace wheelbri::helper {

// What the app last found out about the WheelBri Helper by saying Hello to it (the service's HelperProbe), for the
// Logs tab's Helper row. Portable, so the texts are unit-tested (HelperStatusTest).
struct HelperState {
    enum class Kind {
        NotChecked,   // not asked yet
        NotWindows,   // the helper is a Windows service
        NotRunning,   // no pipe: not installed, or its service is stopped
        Accepted,     // it answered and would do this app's steps
        Refused,      // it answered, but only the installed WheelBri may use it (a development build, say)
        Failed,       // the pipe is there but the Hello did not get through (busy, broken, not the helper's)
    };
    Kind kind = Kind::NotChecked;
    std::string version;   // "0.9.0", from the Hello reply (empty when unknown)
    std::string detail;    // why it refused or failed; the helper's program for Accepted and Refused
};

// What a Hello's outcome means. `reply`: the helper's answer (nullopt when none came, `error` says why);
// `serverImage`: the program that owns the pipe, as Windows names it (empty when unknown).
[[nodiscard]] HelperState helperStateFromHello(const std::optional<Reply>& reply, const std::string& error,
                                               const std::string& serverImage);

// "0.9.0" from "WheelBri Helper 0.9.0" or "WheelBri Helper 0.9.0: <why>"; empty when the text is not that.
[[nodiscard]] std::string helperVersionOf(const std::string& helloText);

// The short text of the Logs tab's Helper row ("running 0.9.0, serves this app", "not running ...", ...); a refusal's
// reason is in the helper's own log ("said Hello but may not use the helper: ...").
[[nodiscard]] std::string describeHelper(const HelperState& state);

}  // namespace wheelbri::helper
