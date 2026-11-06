// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/HelperStatus.hpp"

#include <string_view>

namespace wheelbri::helper {

namespace {

constexpr std::string_view kHelloPrefix = "WheelBri Helper ";

}  // namespace

std::string helperVersionOf(const std::string& helloText) {
    const std::string_view text = helloText;
    if (text.substr(0, kHelloPrefix.size()) != kHelloPrefix) return {};
    std::string_view rest = text.substr(kHelloPrefix.size());
    if (const auto colon = rest.find(": "); colon != std::string_view::npos) rest = rest.substr(0, colon);
    if (rest.empty() || rest.find(' ') != std::string_view::npos) return {};
    return std::string(rest);
}

HelperState helperStateFromHello(const std::optional<Reply>& reply, const std::string& error,
                                 const std::string& serverImage) {
    HelperState state;
    if (!reply) {
        state.kind = HelperState::Kind::Failed;
        state.detail = error.empty() ? std::string("no answer") : error;
        return state;
    }
    state.version = helperVersionOf(reply->text);
    switch (reply->status) {
        case Status::Ok:
            state.kind = HelperState::Kind::Accepted;
            state.detail = serverImage;
            return state;
        case Status::Refused: {
            state.kind = HelperState::Kind::Refused;
            // "WheelBri Helper 0.9.0: only WheelBri (...) may ask": the why after the version.
            const auto colon = reply->text.find(": ");
            state.detail = !state.version.empty() && colon != std::string::npos ? reply->text.substr(colon + 2)
                                                                                : reply->text;
            return state;
        }
        case Status::Failed:
        case Status::Pending:
        case Status::Unsupported: break;
    }
    state.kind = HelperState::Kind::Failed;
    state.detail = std::string(statusName(reply->status)) + (reply->text.empty() ? "" : ": " + reply->text);
    return state;
}

std::string describeHelper(const HelperState& state) {
    const std::string version = state.version.empty() ? std::string() : " " + state.version;
    switch (state.kind) {
        case HelperState::Kind::NotChecked: return "checking...";
        case HelperState::Kind::NotWindows: return "Windows only";
        case HelperState::Kind::NotRunning: return "not running (not installed, or stopped)";
        case HelperState::Kind::Accepted: return "running" + version + ", serves this app";
        case HelperState::Kind::Refused: return "running" + version + ", not for this copy of WheelBri";
        case HelperState::Kind::Failed:
            return "cannot ask it" + (state.detail.empty() ? std::string() : ": " + state.detail);
    }
    return "unknown";
}

}  // namespace wheelbri::helper
