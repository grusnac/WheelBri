#include "HelperServer.hpp"

#include "HelperLog.hpp"
#include "HelperSession.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/HelperProtocol.hpp"
#include "wheelbri/Logging.hpp"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <list>
#include <memory>
#include <thread>
#include <vector>

namespace wheelbri::helper {

namespace {

logging::Logger& log() {
    static logging::Logger logger = logging::get("helper.pipe");
    return logger;
}

std::string windowsError(const DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = "error " + std::to_string(code);
    if (length > 0 && text != nullptr) {
        std::wstring wide(text, length);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L'.')) wide.pop_back();
        message = pathToUtf8(std::filesystem::path(wide)) + " (" + std::to_string(code) + ")";
    }
    if (text != nullptr) LocalFree(text);
    return message;
}

// SYSTEM and Administrators: everything. Interactive users: read and write, but not create another instance
// (FILE_GENERIC_READ | FILE_GENERIC_WRITE without FILE_CREATE_PIPE_INSTANCE = 0x0012019B). Nobody else: no network
// logons, no services of other accounts.
constexpr const wchar_t* kPipeSddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0012019B;;;IU)";

std::wstring pipeNameW() {
    return std::wstring(kPipeName.begin(), kPipeName.end());
}

// Waits for overlapped I/O on `handle` or for `stop`. False when stopped (the I/O is cancelled) or failed (the
// Windows error is left for GetLastError).
bool finish(const HANDLE handle, OVERLAPPED& overlapped, const BOOL started, const HANDLE stop, DWORD& bytes) {
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            SetLastError(error);
            return false;
        }
        const HANDLE waits[2] = {stop, overlapped.hEvent};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(handle, &overlapped);
            GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
            SetLastError(ERROR_OPERATION_ABORTED);
            return false;
        }
    }
    return GetOverlappedResult(handle, &overlapped, &bytes, FALSE) != FALSE;
}

struct Caller {
    DWORD pid = 0;
    ULONG session = 0;
    std::string image;
    bool accepted = false;
    std::string why;            // when not accepted
    HANDLE process = nullptr;   // held for the whole connection: its id cannot be reused meanwhile
};

// Who is at the other end, and whether it may ask for anything (HelperServer.hpp).
Caller identify(const HANDLE pipe, const std::wstring& appFolder) {
    Caller caller;
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) {
        caller.why = "the caller's process is unknown (" + windowsError(GetLastError()) + ")";
        return caller;
    }
    caller.pid = pid;
    // READ_CONTROL | WRITE_DAC: HideRig lets the hide filter (LocalService) look at this process.
    caller.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL | WRITE_DAC, FALSE, pid);
    if (caller.process == nullptr) {
        caller.why = "the caller's process cannot be opened (" + windowsError(GetLastError()) + ")";
        return caller;
    }
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(caller.process, 0, image.data(), &size)) {
        caller.why = "the caller's program cannot be named (" + windowsError(GetLastError()) + ")";
        return caller;
    }
    image.resize(size);
    caller.image = pathToUtf8(std::filesystem::path(image));
    // Still the same process at the other end, and still running: what was named is what is connected.
    ULONG again = 0;
    if (!GetNamedPipeClientProcessId(pipe, &again) || again != pid ||
        WaitForSingleObject(caller.process, 0) != WAIT_TIMEOUT) {
        caller.why = "the caller went away while it was checked";
        return caller;
    }
    if (!GetNamedPipeClientSessionId(pipe, &caller.session) || caller.session == 0) {
        caller.why = "the caller is not in a user's session";
        return caller;
    }
    const std::string folder = pathToUtf8(std::filesystem::path(appFolder));
    if (!isInstalledApp(caller.image, folder)) {
        caller.why = "only WheelBri (wheelbri.exe or wheelbri-service.exe in " + folder +
                     ") may ask the helper for anything, not " + caller.image;
        return caller;
    }
    caller.accepted = true;
    return caller;
}

// The caller's own log folder, %LOCALAPPDATA%\wheelbri\logs, handed to HelperLog with the caller's token; returns
// another copy of that token for the session (ShareLogFolder), or null. Needs a message read first (Windows lets a
// pipe server impersonate only after that).
HANDLE attachCallersLog(const HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) {
        WE_LOG_WARN(log(), "helper: cannot act as the caller to find its log folder: {}", windowsError(GetLastError()));
        return nullptr;
    }
    HANDLE token = nullptr;
    const bool haveToken =
        OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE, FALSE, &token) != FALSE;
    std::wstring folder;
    SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
    if (haveToken) {
        DWORD size = 0;
        GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &size);
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token, &localAppData))) {
            folder = std::wstring(localAppData) + L"\\wheelbri\\logs";
        }
        CoTaskMemFree(localAppData);
    }
    RevertToSelf();
    if (!haveToken || folder.empty() || level < SecurityImpersonation) {
        WE_LOG_WARN(log(), "helper: the caller's log folder is unknown (it must connect allowing impersonation): "
                           "this log stays in memory");
        if (haveToken) CloseHandle(token);
        return nullptr;
    }
    HANDLE forSession = nullptr;
    if (!DuplicateTokenEx(token, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation,
                          &forSession)) {
        forSession = nullptr;
    }
    HelperLog::instance().attachUser(token, folder);
    return forSession;
}

class Connection {
public:
    Connection(const HANDLE pipe, const HANDLE stop, const std::wstring& appFolder, const int scriptLevels)
        : pipe_(pipe), stop_(stop), appFolder_(appFolder), scriptLevels_(scriptLevels) {}

    ~Connection() {
        session_.reset();   // undoes what the caller set up (owns the caller's process handle then)
        if (caller_.process != nullptr) CloseHandle(caller_.process);
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
    }

    void serve() {
        std::vector<std::uint8_t> buffer(kMaxMessage);
        for (;;) {
            DWORD bytes = 0;
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            const BOOL started = ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped);
            const bool read = finish(pipe_, overlapped, started, stop_, bytes);
            const DWORD error = read ? ERROR_SUCCESS : GetLastError();
            CloseHandle(overlapped.hEvent);
            if (!read) {
                if (error == ERROR_MORE_DATA) {
                    WE_LOG_WARN(log(), "helper: pid {} sent more than {} bytes in one message: closing the connection",
                                caller_.pid, kMaxMessage);
                } else if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED &&
                           error != ERROR_PIPE_NOT_CONNECTED) {
                    WE_LOG_WARN(log(), "helper: reading from pid {} failed: {}", caller_.pid, windowsError(error));
                }
                break;
            }
            if (first_) {
                first_ = false;
                caller_ = identify(pipe_, appFolder_);
                if (caller_.accepted) {
                    const HANDLE token = attachCallersLog(pipe_);
                    WE_LOG_INFO(log(), "helper: {} (pid {}, session {}) connected", caller_.image, caller_.pid,
                                caller_.session);
                    // A long request (a driver installer, waiting for a device) gives up when the helper stops or the
                    // caller has gone (Stop in the app closes the connection).
                    const HANDLE stop = stop_;
                    const HANDLE pipe = pipe_;
                    session_ = std::make_unique<HelperSession>(caller_.process, token, scriptLevels_, [stop, pipe] {
                        DWORD available = 0;
                        return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
                               !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr);
                    });
                    caller_.process = nullptr;   // the session's now
                } else {
                    WE_LOG_WARN(log(), "helper: refused a caller (pid {}): {}", caller_.pid, caller_.why);
                }
            }
            bool keepOpen = true;
            const Reply reply = answer(std::span<const std::uint8_t>(buffer.data(), bytes), keepOpen);
            if (!send(reply) || !keepOpen) break;
        }
        if (caller_.accepted) WE_LOG_INFO(log(), "helper: pid {} disconnected", caller_.pid);
        session_.reset();   // removes its virtual G920, shows the rig again
    }

private:
    Reply answer(const std::span<const std::uint8_t> bytes, bool& keepOpen) {
        std::string error;
        const std::optional<Request> request = decodeRequest(bytes, error);
        if (!request) {
            WE_LOG_WARN(log(), "helper: unreadable message from pid {}: {}", caller_.pid, error);
            keepOpen = false;
            return Reply{0, Status::Refused, "unreadable message: " + error};
        }
        const std::string version = std::string("WheelBri Helper ") + WHEELBRI_VERSION;
        if (request->type == RequestType::Hello) {
            return caller_.accepted ? Reply{request->seq, Status::Ok, version}
                                    : Reply{request->seq, Status::Refused, version + ": " + caller_.why};
        }
        if (!caller_.accepted) {
            keepOpen = false;
            return Reply{request->seq, Status::Refused, caller_.why};
        }
        if (!isKnown(request->type)) {
            WE_LOG_WARN(log(), "helper: pid {} asked for unknown request {}", caller_.pid,
                        static_cast<int>(request->type));
            return Reply{request->seq, Status::Unsupported, version + " does not know this request"};
        }
        if (const std::string problem = requestProblem(*request); !problem.empty()) {
            WE_LOG_WARN(log(), "helper: refused {} from pid {}: {}", requestName(request->type), caller_.pid, problem);
            return Reply{request->seq, Status::Refused, problem};
        }
        WE_LOG_INFO(log(), "helper: {} from pid {}", requestName(request->type), caller_.pid);
        Reply reply = session_->handle(*request);
        if (reply.status != Status::Ok) {
            WE_LOG_WARN(log(), "helper: {} for pid {}: {}: {}", requestName(request->type), caller_.pid,
                        statusName(reply.status), reply.text);
        }
        return reply;
    }

    bool send(const Reply& reply) {
        const std::vector<std::uint8_t> bytes = encode(reply);
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        const BOOL started = WriteFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &overlapped);
        const bool ok = finish(pipe_, overlapped, started, stop_, written) && written == bytes.size();
        CloseHandle(overlapped.hEvent);
        return ok;
    }

    HANDLE pipe_;
    HANDLE stop_;
    const std::wstring& appFolder_;
    int scriptLevels_;
    Caller caller_;
    std::unique_ptr<HelperSession> session_;
    bool first_ = true;
};

struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace

HelperServer::HelperServer(std::wstring appFolder, const int scriptLevels)
    : appFolder_(std::move(appFolder)), scriptLevels_(scriptLevels) {}

bool HelperServer::run(void* const stopEvent, std::string& error) {
    const HANDLE stop = static_cast<HANDLE>(stopEvent);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        error = "cannot make the pipe's permissions: " + windowsError(GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const std::wstring name = pipeNameW();
    std::list<Worker> workers;
    bool first = true;
    bool ok = true;
    WE_LOG_INFO(log(), "helper: listening on {} for WheelBri in {}", kPipeName,
                pathToUtf8(std::filesystem::path(appFolder_)));

    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        workers.remove_if([](Worker& w) {
            if (!w.done->load()) return false;
            w.thread.join();
            return true;
        });
        const HANDLE pipe = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, kMaxConnections,
            static_cast<DWORD>(kMaxMessage), static_cast<DWORD>(kMaxMessage), 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (first) {
                error = code == ERROR_ACCESS_DENIED
                            ? std::string("the pipe exists already: another helper (or a program posing as one) runs")
                            : "cannot create the pipe: " + windowsError(code);
                ok = false;
                break;
            }
            // All kMaxConnections in use: wait for one to end.
            if (WaitForSingleObject(stop, 250) == WAIT_OBJECT_0) break;
            continue;
        }
        first = false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD unused = 0;
        BOOL connected = ConnectNamedPipe(pipe, &overlapped);
        if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = TRUE;   // the client was quicker than ConnectNamedPipe
        } else {
            connected = finish(pipe, overlapped, connected, stop, unused) ? TRUE : FALSE;
        }
        CloseHandle(overlapped.hEvent);
        if (!connected) {
            CloseHandle(pipe);
            continue;   // stopped, or the client gave up
        }
        Worker& worker = workers.emplace_back();
        worker.thread = std::thread([pipe, stop, this, done = worker.done] {
            {
                Connection connection(pipe, stop, appFolder_, scriptLevels_);
                connection.serve();
            }
            done->store(true);
        });
    }
    for (Worker& worker : workers) worker.thread.join();
    LocalFree(descriptor);
    WE_LOG_INFO(log(), "helper: stopped listening");
    return ok;
}

}  // namespace wheelbri::helper
