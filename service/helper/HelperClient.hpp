// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wheelbri/HelperProtocol.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::helper {

// The app's side of the pipe: one connection for as long as the app needs the helper (closing it makes the helper
// undo what it set up). Windows only. Used by `wheelbri-helper --hello` now and by the app from phase 4.
class HelperClient {
public:
    enum class Connect { Ok, NoHelper, Failed };

    // How long connect waits for a free connection when all of the helper's are in use.
    static constexpr int kBusyWaitMs = 5000;

    // `pipeName`: the helper's pipe (empty: kPipeName; the tests use their own).
    explicit HelperClient(std::wstring pipeName = {});
    ~HelperClient();
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;

    // Opens the pipe, letting the helper act as this user (it needs that for the user's log folder) and only on
    // this computer. NoHelper: no helper is running (not installed, or its service is stopped). With
    // `expectedFolder` (UTF-8), the program at the other end must be wheelbri-helper.exe in that folder (the app's
    // own): a program of the user that created the pipe while no helper ran is refused.
    Connect connect(std::string& error, const std::string& expectedFolder = {});

    // Sends one request and waits for its reply (at most `timeoutMs`). Nullopt with `error` when the pipe broke,
    // the reply did not come in time, it could not be read, or `cancelled` said so (checked every 100 ms); the
    // connection is then closed, which makes the helper stop the request and undo what this connection set up.
    std::optional<Reply> call(RequestType type, const std::vector<std::string>& devices, int timeoutMs,
                              std::string& error, const std::function<bool()>& cancelled = {});

    // The helper's process id and program, as Windows names them (for the log; empty when unknown).
    [[nodiscard]] std::uint32_t serverPid() const { return serverPid_; }
    [[nodiscard]] const std::string& serverImage() const { return serverImage_; }

    void close();

private:
    std::wstring pipeName_;
    void* pipe_ = nullptr;   // HANDLE
    std::uint32_t seq_ = 0;
    std::uint32_t serverPid_ = 0;
    std::string serverImage_;
};

}  // namespace wheelbri::helper
