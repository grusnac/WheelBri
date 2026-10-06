#include "HelperClient.hpp"

#include "wheelbri/FileText.hpp"

#include <windows.h>

#include <filesystem>
#include <vector>

namespace wheelbri::helper {

namespace {

// Overlapped I/O with a time limit; false (and the I/O cancelled) when it fails or takes longer.
bool finishWithin(const HANDLE handle, OVERLAPPED& overlapped, const BOOL started, const int timeoutMs, DWORD& bytes,
                  DWORD& error) {
    if (!started) {
        error = GetLastError();
        if (error != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
            CancelIoEx(handle, &overlapped);
            GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
            error = WAIT_TIMEOUT;
            return false;
        }
    }
    if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
        error = GetLastError();
        return false;
    }
    return true;
}

}  // namespace

HelperClient::~HelperClient() {
    close();
}

void HelperClient::close() {
    if (pipe_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
    }
}

HelperClient::Connect HelperClient::connect(std::string& error) {
    close();
    const std::wstring name(kPipeName.begin(), kPipeName.end());
    for (int attempt = 0; attempt < 2; ++attempt) {
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
                if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                    std::wstring image(32768, L'\0');
                    DWORD size = static_cast<DWORD>(image.size());
                    if (QueryFullProcessImageNameW(process, 0, image.data(), &size)) {
                        image.resize(size);
                        serverImage_ = pathToUtf8(std::filesystem::path(image));
                    }
                    CloseHandle(process);
                }
            }
            return Connect::Ok;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            error = "the WheelBri Helper is not running";
            return Connect::NoHelper;
        }
        if (code != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 2000)) {
            error = "cannot open the helper's pipe (error " + std::to_string(code) + ")";
            return Connect::Failed;
        }
    }
    error = "the helper is busy";
    return Connect::Failed;
}

std::optional<Reply> HelperClient::call(const RequestType type, const std::vector<std::string>& devices,
                                        const int timeoutMs, std::string& error) {
    if (pipe_ == nullptr) {
        error = "not connected to the helper";
        return std::nullopt;
    }
    const HANDLE pipe = static_cast<HANDLE>(pipe_);
    const Request request{type, ++seq_, devices};
    const std::vector<std::uint8_t> bytes = encode(request);
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD done = 0;
    DWORD code = 0;
    bool ok = finishWithin(pipe, overlapped,
                           WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &overlapped),
                           timeoutMs, done, code);
    std::vector<std::uint8_t> buffer(kMaxMessage);
    if (ok) {
        ResetEvent(overlapped.hEvent);
        ok = finishWithin(pipe, overlapped,
                          ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped),
                          timeoutMs, done, code);
    }
    CloseHandle(overlapped.hEvent);
    if (!ok) {
        error = code == WAIT_TIMEOUT ? "the helper did not answer within " + std::to_string(timeoutMs / 1000) + " s"
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
