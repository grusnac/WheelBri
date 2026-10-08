#include "wheelbri/HelperDispatch.hpp"

#include "wheelbri/Logging.hpp"

#include <exception>

namespace wheelbri::helper {

namespace {

logging::Logger& log() {
    static logging::Logger logger = logging::get("helper.pipe");
    return logger;
}

}  // namespace

Answer answerMessage(const std::span<const std::uint8_t> bytes, const CallerInfo& caller, const std::string_view version,
                     RequestHandler* const session) {
    Answer answer;
    std::string error;
    const std::optional<Request> request = decodeRequest(bytes, error);
    if (!request) {
        WE_LOG_WARN(log(), "helper: unreadable message from pid {}: {}", caller.pid, error);
        answer.reply = Reply{0, Status::Refused, "unreadable message: " + error};
        answer.keepOpen = false;
        return answer;
    }
    answer.type = request->type;
    const std::uint32_t seq = request->seq;
    if (request->type == RequestType::Hello) {
        if (mayTakeTheLog(caller)) answer.logLevel = requestLogLevel(*request);
        if (caller.accepted) {
            answer.reply = Reply{seq, Status::Ok, std::string(version)};
        } else {
            answer.reply = Reply{seq, Status::Refused, std::string(version) + ": " + caller.why};
            answer.keepOpen = false;
        }
        return answer;
    }
    if (!caller.accepted) {
        WE_LOG_WARN(log(), "helper: refused {} from pid {}: {}", requestName(request->type), caller.pid, caller.why);
        answer.reply = Reply{seq, Status::Refused, caller.why};
        answer.keepOpen = false;
        return answer;
    }
    if (!isKnown(request->type)) {
        WE_LOG_WARN(log(), "helper: pid {} asked for unknown request {}", caller.pid,
                    static_cast<int>(request->type));
        answer.reply = Reply{seq, Status::Unsupported, std::string(version) + " does not know this request"};
        return answer;
    }
    if (const std::string problem = requestProblem(*request); !problem.empty()) {
        WE_LOG_WARN(log(), "helper: refused {} from pid {}: {}", requestName(request->type), caller.pid, problem);
        answer.reply = Reply{seq, Status::Refused, problem};
        return answer;
    }
    WE_LOG_INFO(log(), "helper: {} from pid {}", requestName(request->type), caller.pid);
    if (session == nullptr) {
        answer.reply = Reply{seq, Status::Failed, "the helper has no session for this connection"};
    } else {
        try {
            answer.reply = session->handle(*request);
            answer.reply.seq = seq;
        } catch (const std::exception& e) {
            answer.reply = Reply{seq, Status::Failed, std::string("the helper failed: ") + e.what()};
        } catch (...) {
            answer.reply = Reply{seq, Status::Failed, "the helper failed (an unknown error)"};
        }
    }
    if (answer.reply.status == Status::Ok && request->type == RequestType::SetLogLevel) {
        answer.logLevel = requestLogLevel(*request);
    }
    if (answer.reply.status != Status::Ok) {
        if (answer.reply.status == Status::Failed) {
            WE_LOG_ERROR(log(), "helper: {} for pid {}: {}: {}", requestName(request->type), caller.pid,
                         statusName(answer.reply.status), answer.reply.text);
        } else {
            WE_LOG_WARN(log(), "helper: {} for pid {}: {}: {}", requestName(request->type), caller.pid,
                        statusName(answer.reply.status), answer.reply.text);
        }
    }
    return answer;
}

bool mayTakeTheLog(const CallerInfo& caller) {
    return caller.accepted || isWheelBriProgram(caller.image);
}

}  // namespace wheelbri::helper
