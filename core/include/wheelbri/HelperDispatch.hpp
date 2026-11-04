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
    // The log level the helper takes for its own log: a Hello's (from a caller that may take the log) or an accepted
    // SetLogLevel's. Nullopt: its level stays.
    std::optional<LogLevel> logLevel;
};

// The rules, in this order:
// - a message that cannot be read: Refused (seq 0), and the connection is closed;
// - Hello: Ok with `version` for an accepted caller (the connection stays); for any other caller Refused with
//   "<version>: <why>", and the connection is closed (it may hold none of the helper's few connections). Its log
//   level, when it carries one and the caller may take the log (mayTakeTheLog), is the Answer's logLevel;
// - anything but Hello from a caller not accepted: Refused with why, closed;
// - an unknown request type: Unsupported (an app newer than the helper), the connection stays;
// - a request requestProblem refuses: Refused with the problem, the connection stays;
// - otherwise `session` carries it out; no session (it could not be made) or a session that throws: Failed, with
//   what went wrong, the connection stays (and the helper keeps running). A SetLogLevel the session carried out
//   (Ok) sets the Answer's logLevel too.
// Every refusal and failure is logged (helper.pipe) with the caller's pid.
[[nodiscard]] Answer answerMessage(std::span<const std::uint8_t> bytes, const CallerInfo& caller,
                                   std::string_view version, RequestHandler* session);

// Whether the helper's log may move into this caller's own log folder (written as the caller): an accepted caller
// only. A program merely named wheelbri.exe elsewhere (anyone may name one so) may not: on a PC with several users it
// would take the log, and every other user's lines in it, into its own folder. A development build's app takes it
// from a helper of the same build (--console), which accepts it.
[[nodiscard]] bool mayTakeTheLog(const CallerInfo& caller);

}  // namespace wheelbri::helper
