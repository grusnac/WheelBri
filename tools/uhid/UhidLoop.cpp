#include "UhidLoop.hpp"

#include <algorithm>

namespace wheelbri::uhid {

namespace {

std::optional<std::uint16_t> parsePort(const std::string& text) {
    if (text.empty() || text.size() > 5 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    const unsigned long value = std::stoul(text);
    if (value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

std::optional<int> parsePid(const std::string& text) {
    if (text.empty() || text.size() > 9 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    const int value = std::stoi(text);
    if (value <= 1) {   // 1 is init: every orphan's parent, so "the parent is gone" could never be seen
        return std::nullopt;
    }
    return value;
}

}  // namespace

ParsedOptions parseOptions(const std::vector<std::string>& args) {
    ParsedOptions result;
    Options o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-h" || arg == "--help") {
            return result;
        }
        const bool takesValue = arg == "--state-port" || arg == "--ffb-port" || arg == "--stop-file" ||
                                arg == "--device" || arg == "--parent";
        if (!takesValue) {
            result.error = "unknown option " + arg;
            return result;
        }
        if (i + 1 >= args.size()) {
            result.error = arg + " needs a value";
            return result;
        }
        const std::string& value = args[++i];
        if (arg == "--state-port" || arg == "--ffb-port") {
            const auto port = parsePort(value);
            if (!port) {
                result.error = arg + ": '" + value + "' is not a port (1..65535)";
                return result;
            }
            (arg == "--state-port" ? o.statePort : o.ffbPort) = *port;
        } else if (arg == "--parent") {
            const auto pid = parsePid(value);
            if (!pid) {
                result.error = "--parent: '" + value + "' is not a process id (2 or more)";
                return result;
            }
            o.parentPid = *pid;
        } else if (value.empty()) {
            result.error = arg + " needs a value";
            return result;
        } else if (arg == "--stop-file") {
            o.stopFile = value;
        } else {
            o.device = value;
        }
    }
    if (o.statePort == o.ffbPort) {
        result.error = "--state-port and --ffb-port are both " + std::to_string(o.statePort);
        return result;
    }
    result.options = o;
    return result;
}

InputFeed::Change InputFeed::update(const std::optional<StatePacket>& packet, const std::int64_t nowMs,
                                    UhidG920& device) {
    Change change = Change::None;
    if (packet) {
        if (!receiving_) {
            receiving_ = true;
            change = Change::Receiving;
        }
        lastState_ = nowMs;
        neutralSent_ = false;
        current_ = packet->report;
        // The keep-alive counts from the last report written, not the last state: the service repeats an unchanged
        // state every 100 ms, and counting those kept the report from ever being sent again (until 2026-10-08).
        if (device.submit(current_, nowMs - lastInput_ >= kKeepAliveMs)) {
            lastInput_ = nowMs;
        }
    } else if (nowMs - lastState_ > kStaleMs && !neutralSent_) {
        if (receiving_) {
            change = Change::Stale;
        }
        receiving_ = false;
        neutralSent_ = true;
        current_ = g920Neutral();
        device.submit(current_, true);
        lastInput_ = nowMs;
    } else if (nowMs - lastInput_ >= kKeepAliveMs) {
        device.submit(current_, true);
        lastInput_ = nowMs;
    }
    return change;
}

std::int64_t InputFeed::msUntilDue(const std::int64_t nowMs) const {
    std::int64_t due = lastInput_ + kKeepAliveMs;
    if (!neutralSent_) {
        due = std::min(due, lastState_ + kStaleMs + 1);   // update() goes neutral once more than kStaleMs passed
    }
    return std::max<std::int64_t>(0, due - nowMs);
}

}  // namespace wheelbri::uhid
