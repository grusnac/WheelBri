// SPDX-License-Identifier: GPL-3.0-or-later
#include "HelperClient.hpp"

#include "Platform.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/HideIds.hpp"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace wheelbri::helper {

namespace {

// Overlapped I/O with a time limit; false (and the I/O cancelled) when it fails, takes longer or is cancelled
// (ERROR_CANCELLED).
bool finishWithin(const HANDLE handle, OVERLAPPED& overlapped, const BOOL started, const int timeoutMs,
                  const std::function<bool()>& cancelled, DWORD& bytes, DWORD& error) {
    if (!started) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) return false;
        const ULONGLONG start = GetTickCount64();
        for (;;) {
            const DWORD waited = WaitForSingleObject(overlapped.hEvent, 100);
            if (waited == WAIT_OBJECT_0) break;
            const bool failed = waited == WAIT_FAILED;
            const DWORD waitError = failed ? GetLastError() : 0;
            const bool stop = !failed && cancelled && cancelled();
            const bool late = GetTickCount64() - start >= static_cast<ULONGLONG>(timeoutMs < 0 ? 0 : timeoutMs);
            if (failed || stop || late) {
                CancelIoEx(handle, &overlapped);
                GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
                error = failed ? waitError : stop ? ERROR_CANCELLED : WAIT_TIMEOUT;
                return false;
            }
        }
    }
    if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
        error = GetLastError();
        return false;
    }
    return true;
}

}  // namespace

HelperClient::HelperClient(std::wstring pipeName) : pipeName_(std::move(pipeName)) {
    if (pipeName_.empty()) pipeName_ = std::wstring(kPipeName.begin(), kPipeName.end());
}

HelperClient::~HelperClient() {
    close();
}

void HelperClient::close() {
    if (pipe_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
}

HelperClient::Connect HelperClient::connect(std::string& error, const std::string& expectedFolder) {
    close();
    const std::wstring& name = pipeName_;
    // All of the helper's connections in use: wait for one to come free, up to kBusyWaitMs in all (another program
    // may take a freed one first; then wait again).
    const ULONGLONG deadline = GetTickCount64() + kBusyWaitMs;
    for (;;) {
        const HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                error = "cannot switch the pipe to messages (error " + std::to_string(GetLastError()) + ")";
                CloseHandle(pipe);
                return Connect::Failed;
            }
            pipe_ = pipe;
            ULONG pid = 0;
            if (GetNamedPipeServerProcessId(pipe, &pid)) {
                serverPid_ = pid;
                // A standard user may not open a service's process (seen 2026-10-06: the installed helper was "a
                // program Windows will not name" and every Start was refused); processImage then names it without
                // opening it (NtQuerySystemInformation), as for WUDFHost.exe.
                serverImage_ = pathToUtf8(service::platform::processImage(pid));
            }
            if (!expectedFolder.empty() &&
                !hideids::equalsIgnoreCase(serverImage_, expectedFolder + "\\wheelbri-helper.exe")) {
                error = "the helper's pipe belongs to " + (serverImage_.empty() ? std::string("a program Windows will not name")
                                                                                : serverImage_) +
                        ", not to " + expectedFolder + "\\wheelbri-helper.exe";
                close();
                return Connect::Failed;
            }
            return Connect::Ok;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            error = "the WheelBri Helper is not running";
            return Connect::NoHelper;
        }
        if (code == ERROR_ACCESS_DENIED) {
            error = "the helper's pipe may not be opened by this user (error 5): not the logged-on user, or not the "
                    "helper's pipe";
            return Connect::Failed;
        }
        if (code != ERROR_PIPE_BUSY) {
            error = "cannot open the helper's pipe (error " + std::to_string(code) + ")";
            return Connect::Failed;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        // Returns when an instance is free (or at once, or with an error, on some systems): then try again, and
        // sleep a little so a quick refusal does not spin.
        if (!WaitNamedPipeW(name.c_str(), static_cast<DWORD>(deadline - now))) Sleep(50);
    }
    error = "the helper is busy";
    return Connect::Failed;
}

std::optional<Reply> HelperClient::call(const RequestType type, const std::vector<std::string>& devices,
                                        const int timeoutMs, std::string& error,
                                        const std::function<bool()>& cancelled) {
    if (pipe_ == nullptr) {
        error = "not connected to the helper";
        return std::nullopt;
    }
    const HANDLE pipe = static_cast<HANDLE>(pipe_);
    const Request request{type, ++seq_, devices};
    const std::vector<std::uint8_t> bytes = encode(request);
    if (bytes.size() > kMaxMessage) {
        error = "the request is longer than the helper reads (" + std::to_string(bytes.size()) + " bytes)";
        return std::nullopt;   // nothing was sent: the connection is still good
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        error = "no event to wait for the helper (error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    DWORD done = 0;
    DWORD code = 0;
    bool ok = finishWithin(pipe, overlapped,
                           WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &overlapped),
                           timeoutMs, cancelled, done, code);
    std::vector<std::uint8_t> buffer(kMaxMessage);
    if (ok) {
        ResetEvent(overlapped.hEvent);
        ok = finishWithin(pipe, overlapped,
                          ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped),
                          timeoutMs, cancelled, done, code);
    }
    CloseHandle(overlapped.hEvent);
    if (!ok) {
        error = code == WAIT_TIMEOUT      ? "the helper did not answer within " +
                                                (timeoutMs % 1000 == 0 ? std::to_string(timeoutMs / 1000) + " s"
                                                                       : std::to_string(timeoutMs) + " ms")
                : code == ERROR_CANCELLED ? std::string("stopped")
                                          : "the connection to the helper broke (error " + std::to_string(code) + ")";
        close();
        return std::nullopt;
    }
    std::optional<Reply> reply = decodeReply(std::span<const std::uint8_t>(buffer.data(), done), error);
    if (!reply) {
        error = "unreadable reply from the helper: " + error;
        close();
        return std::nullopt;
    }
    if (reply->seq != request.seq && reply->seq != 0) {
        error = "the helper answered another request";
        close();
        return std::nullopt;
    }
    return reply;
}

}  // namespace wheelbri::helper
