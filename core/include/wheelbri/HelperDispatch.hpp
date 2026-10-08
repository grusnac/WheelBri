#pragma once

#include "wheelbri/HelperProtocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wheelbri::helper {

// How the WheelBri Helper answers one message on a connection, apart from the pipe itself (HelperServer) and the
// steps themselves (HelperSession): portable, so every rule below is unit-tested (HelperDispatchTest).

// Who is at the other end of a connection, as the server found out (HelperServer.cpp identify).
struct CallerInfo {
    std::uint32_t pid = 0;
    std::string image;          // its program, as Windows names it (empty when unknown)
    bool accepted = false;      // WheelBri in the helper's own folder: may ask for everything
    std::string why;            // when not accepted
};

// What carries the requests out (HelperSession in the helper, a fake in the tests).
class RequestHandler {
public:
    virtual ~RequestHandler() = default;
    // A request that passed requestProblem, not Hello. May throw: the answer is then Failed.
    [[nodiscard]] virtual Reply handle(const Request& request) = 0;
};

struct Answer {
    Reply reply;
    bool keepOpen = true;                   // false: close the connection after sending the reply
    std::optional<RequestType> type;        // the request's type, when the message could be read
};

// The rules, in this order:
// - a message that cannot be read: Refused (seq 0), and the connection is closed;
// - Hello: Ok with `version` for an accepted caller (the connection stays); for any other caller Refused with
//   "<version>: <why>", and the connection is closed (it may hold none of the helper's few connections);
// - anything but Hello from a caller not accepted: Refused with why, closed;
// - an unknown request type: Unsupported (an app newer than the helper), the connection stays;
// - a request requestProblem refuses: Refused with the problem, the connection stays;
// - otherwise `session` carries it out; no session (it could not be made) or a session that throws: Failed, with
//   what went wrong, the connection stays (and the helper keeps running).
// Every refusal and failure is logged (helper.pipe) with the caller's pid.
[[nodiscard]] Answer answerMessage(std::span<const std::uint8_t> bytes, const CallerInfo& caller,
                                   std::string_view version, RequestHandler* session);

// Whether the helper's log may move into this caller's own log folder (written as the caller): an accepted caller,
// or WheelBri itself from another folder (a development build, or an app run as Administrator) saying Hello. Any
// other program may not take the helper's log.
[[nodiscard]] bool mayTakeTheLog(const CallerInfo& caller);

}  // namespace wheelbri::helper
