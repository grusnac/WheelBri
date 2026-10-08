#include "HelperServer.hpp"

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
#include <mutex>
#include <set>
#include <system_error>
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

// An event for overlapped I/O, closed with this object (null when Windows had none to give).
class IoEvent {
public:
    IoEvent() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~IoEvent() {
        if (event_ != nullptr) CloseHandle(event_);
    }
    IoEvent(const IoEvent&) = delete;
    IoEvent& operator=(const IoEvent&) = delete;
    [[nodiscard]] HANDLE get() const { return event_; }

private:
    HANDLE event_;
};

// Waits for overlapped I/O on `handle`, for `stop` or for `timeoutMs` (INFINITE: no limit). False when stopped,
// timed out (the I/O is cancelled) or failed: the Windows error is left for GetLastError (ERROR_OPERATION_ABORTED when
// stopped, WAIT_TIMEOUT when it took too long).
bool finish(const HANDLE handle, OVERLAPPED& overlapped, const BOOL started, const HANDLE stop, const DWORD timeoutMs,
            DWORD& bytes) {
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            SetLastError(error);
            return false;
        }
        const HANDLE waits[2] = {stop, overlapped.hEvent};
        const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
        if (waited != WAIT_OBJECT_0 + 1) {
            const DWORD why = waited == WAIT_TIMEOUT ? WAIT_TIMEOUT
                              : waited == WAIT_FAILED ? GetLastError()
                                                      : ERROR_OPERATION_ABORTED;
            CancelIoEx(handle, &overlapped);
            GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
            SetLastError(why);
            return false;
        }
    }
    return GetOverlappedResult(handle, &overlapped, &bytes, FALSE) != FALSE;
}

struct Caller {
    CallerInfo info;
    ULONG session = 0;
    HANDLE process = nullptr;   // held for the whole connection: its id cannot be reused meanwhile
};

// Who is at the other end, and whether it may ask for anything (HelperServer.hpp).
Caller identify(const HANDLE pipe, const HelperServer::Options& options) {
    Caller caller;
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) {
        caller.info.why = "the caller's process is unknown (" + windowsError(GetLastError()) + ")";
        return caller;
    }
    caller.info.pid = pid;
    // READ_CONTROL | WRITE_DAC: HideRig lets the hide filter (LocalService) look at this process.
    caller.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | READ_CONTROL | WRITE_DAC, FALSE, pid);
    if (caller.process == nullptr) {
        caller.info.why = "the caller's process cannot be opened (" + windowsError(GetLastError()) + ")";
        return caller;
    }
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(caller.process, 0, image.data(), &size)) {
        caller.info.why = "the caller's program cannot be named (" + windowsError(GetLastError()) + ")";
        return caller;
    }
    image.resize(size);
    caller.info.image = pathToUtf8(std::filesystem::path(image));
    // Still the same process at the other end, and still running: what was named is what is connected.
    ULONG again = 0;
    if (!GetNamedPipeClientProcessId(pipe, &again) || again != pid ||
        WaitForSingleObject(caller.process, 0) != WAIT_TIMEOUT) {
        caller.info.why = "the caller went away while it was checked";
        return caller;
    }
    if (!GetNamedPipeClientSessionId(pipe, &caller.session) || caller.session == 0) {
        caller.info.why = "the caller is not in a user's session";
        return caller;
    }
    const std::string folder = pathToUtf8(std::filesystem::path(options.appFolder));
    const bool isApp = options.isApp ? options.isApp(caller.info.image) : isInstalledApp(caller.info.image, folder);
    if (!isApp) {
        caller.info.why = "only WheelBri (wheelbri.exe or wheelbri-service.exe in " + folder +
                          ") may ask the helper for anything, not " + caller.info.image;
        return caller;
    }
    caller.info.accepted = true;
    return caller;
}

// The caller's own log folder, %LOCALAPPDATA%\wheelbri\logs, handed to `takeLog` with the caller's token. Returns
// another copy of that token for the session (ShareLogFolder) when `forSession`, else null. Needs a message read
// first (Windows lets a pipe server impersonate only after that).
HANDLE attachCallersLog(const HANDLE pipe, const HelperServer::LogTaker& takeLog, const bool forSession) {
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
        if (!GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &size)) {
            level = SecurityAnonymous;
        }
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token, &localAppData))) {
            folder = std::wstring(localAppData) + L"\\wheelbri\\logs";
        }
        CoTaskMemFree(localAppData);
    }
    RevertToSelf();
    if (!haveToken || folder.empty() || level < SecurityImpersonation) {
        WE_LOG_WARN(log(), "helper: the caller's log folder is unknown (it must connect allowing impersonation): "
                           "this log stays where it is");
        if (haveToken) CloseHandle(token);
        return nullptr;
    }
    HANDLE copy = nullptr;
    if (forSession && !DuplicateTokenEx(token, TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr, SecurityImpersonation,
                                        TokenImpersonation, &copy)) {
        copy = nullptr;
    }
    if (takeLog) {
        takeLog(token, folder);
    } else {
        CloseHandle(token);
    }
    return copy;
}

// The first Hello of each program is worth an info line; the app asks again from time to time (debug).
class HelloNotes {
public:
    bool firstFrom(const std::string& image) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (seen_.count(image) != 0) return false;
        if (seen_.size() >= 32) seen_.clear();
        seen_.insert(image);
        return true;
    }

private:
    std::mutex mutex_;
    std::set<std::string> seen_;
};

class Connection {
public:
    Connection(const HANDLE pipe, const HANDLE stop, const HelperServer::Options& options, HelloNotes& hellos)
        : pipe_(pipe), stop_(stop), options_(options), hellos_(hellos) {}

    ~Connection() {
        session_.reset();   // undoes what the caller set up (it owns the caller's process handle then)
        if (caller_.process != nullptr) CloseHandle(caller_.process);
        DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void serve() {
        std::vector<std::uint8_t> buffer(kMaxMessage);
        for (;;) {
            DWORD bytes = 0;
            if (!read(buffer, bytes)) break;
            if (first_) {
                first_ = false;
                begin();
            }
            const Answer answer = answerMessage(std::span<const std::uint8_t>(buffer.data(), bytes), caller_.info,
                                                options_.version, session_.get());
            note(answer);
            if (answer.logLevel && options_.setLogLevel) options_.setLogLevel(*answer.logLevel);
            if (!send(answer.reply) || !answer.keepOpen) break;
        }
        if (announced_) WE_LOG_INFO(log(), "helper: pid {} disconnected", caller_.info.pid);
        session_.reset();   // removes its virtual G920, shows the rig again
    }

private:
    bool read(std::vector<std::uint8_t>& buffer, DWORD& bytes) {
        const IoEvent event;
        if (event.get() == nullptr) {
            WE_LOG_ERROR(log(), "helper: no event for reading from pid {}: {}", caller_.info.pid,
                         windowsError(GetLastError()));
            return false;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        const BOOL started = ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped);
        // The first message must come soon; an accepted caller may then stay quiet for its whole session.
        const DWORD limit = first_ ? static_cast<DWORD>(options_.firstMessageMs) : INFINITE;
        if (finish(pipe_, overlapped, started, stop_, limit, bytes)) return true;
        const DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA) {
            WE_LOG_WARN(log(), "helper: pid {} sent more than {} bytes in one message: closing the connection",
                        caller_.info.pid, kMaxMessage);
        } else if (error == WAIT_TIMEOUT) {
            ULONG pid = 0;
            GetNamedPipeClientProcessId(pipe_, &pid);
            WE_LOG_WARN(log(), "helper: pid {} connected but said nothing for {} s: closing the connection", pid,
                        options_.firstMessageMs / 1000);
        } else if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED && error != ERROR_PIPE_NOT_CONNECTED) {
            WE_LOG_WARN(log(), "helper: reading from pid {} failed: {}", caller_.info.pid, windowsError(error));
        }
        return false;
    }

    // After the first message: who it is, where the log goes, and an accepted caller's session.
    void begin() {
        caller_ = identify(pipe_, options_);
        HANDLE token = nullptr;
        if (mayTakeTheLog(caller_.info)) token = attachCallersLog(pipe_, options_.takeLog, caller_.info.accepted);
        if (!caller_.info.accepted) {
            if (token != nullptr) CloseHandle(token);
            return;
        }
        if (!options_.makeSession) {
            if (token != nullptr) CloseHandle(token);
            return;   // the process handle is closed with the connection
        }
        // A long request (a driver installer, waiting for a device) gives up when the helper stops or the caller has
        // gone (Stop in the app closes the connection).
        const HANDLE stop = stop_;
        const HANDLE pipe = pipe_;
        const HANDLE process = caller_.process;
        caller_.process = nullptr;   // the factory's now, whatever happens
        try {
            session_ = options_.makeSession(process, token, [stop, pipe] {
                DWORD available = 0;
                return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
                       !PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr);
            });
        } catch (const std::exception& e) {
            WE_LOG_ERROR(log(), "helper: no session for pid {}: {}", caller_.info.pid, e.what());
        } catch (...) {
            WE_LOG_ERROR(log(), "helper: no session for pid {} (an unknown error)", caller_.info.pid);
        }
    }

    void note(const Answer& answer) {
        if (answer.type == RequestType::Hello) {
            const bool first = hellos_.firstFrom(caller_.info.image);
            if (caller_.info.accepted) {
                if (first) {
                    WE_LOG_INFO(log(), "helper: {} (pid {}) said Hello", caller_.info.image, caller_.info.pid);
                } else {
                    WE_LOG_DEBUG(log(), "helper: {} (pid {}) said Hello", caller_.info.image, caller_.info.pid);
                }
            } else if (first) {
                WE_LOG_INFO(log(), "helper: pid {} said Hello but may not use the helper: {}", caller_.info.pid,
                            caller_.info.why);
            } else {
                WE_LOG_DEBUG(log(), "helper: pid {} said Hello but may not use the helper: {}", caller_.info.pid,
                             caller_.info.why);
            }
            return;
        }
        if (caller_.info.accepted && !announced_ && answer.type.has_value()) {
            announced_ = true;
            WE_LOG_INFO(log(), "helper: {} (pid {}, session {}) connected", caller_.info.image, caller_.info.pid,
                        caller_.session);
        }
    }

    bool send(const Reply& reply) {
        const std::vector<std::uint8_t> bytes = encode(reply);
        const IoEvent event;
        if (event.get() == nullptr) return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD written = 0;
        const BOOL started = WriteFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &overlapped);
        if (finish(pipe_, overlapped, started, stop_, HelperServer::kWriteMs, written) && written == bytes.size()) {
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED && error != ERROR_NO_DATA) {
            WE_LOG_WARN(log(), "helper: answering pid {} failed: {}", caller_.info.pid, windowsError(error));
        }
        return false;
    }

    HANDLE pipe_;
    HANDLE stop_;
    const HelperServer::Options& options_;
    HelloNotes& hellos_;
    Caller caller_;
    std::unique_ptr<RequestHandler> session_;
    bool first_ = true;
    bool announced_ = false;   // "connected" logged (at its first request other than Hello)
};

struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace

HelperServer::HelperServer(Options options) : options_(std::move(options)) {
    if (options_.pipeName.empty()) options_.pipeName = std::wstring(kPipeName.begin(), kPipeName.end());
}

bool HelperServer::run(void* const stopEvent, std::string& error) {
    const HANDLE stop = static_cast<HANDLE>(stopEvent);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        error = "cannot make the pipe's permissions: " + windowsError(GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    // A worker sets it when its connection has ended, so a full server takes the next caller at once.
    const HANDLE freed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::list<Worker> workers;
    HelloNotes hellos;
    bool first = true;
    bool ok = true;
    DWORD lastCreateError = ERROR_SUCCESS;   // logged once until a pipe is made again
    HANDLE listening = INVALID_HANDLE_VALUE;
    WE_LOG_INFO(log(), "helper: listening on {} for WheelBri in {}", pathToUtf8(std::filesystem::path(options_.pipeName)),
                pathToUtf8(std::filesystem::path(options_.appFolder)));

    // One instance more than the connections served: there is always one that is not serving anybody, so the pipe's
    // name never goes away while the helper runs (a caller then finds it busy, not missing, and no other program can
    // create the name in between). While kMaxConnections are served, that one waits unconnected.
    while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        workers.remove_if([](Worker& w) {
            if (!w.done->load()) return false;
            w.thread.join();
            return true;
        });
        if (listening == INVALID_HANDLE_VALUE) {
            listening = CreateNamedPipeW(
                options_.pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, kMaxConnections + 1,
                static_cast<DWORD>(kMaxMessage), static_cast<DWORD>(kMaxMessage), 0, &attributes);
            if (listening == INVALID_HANDLE_VALUE) {
                const DWORD code = GetLastError();
                if (first) {
                    error = code == ERROR_ACCESS_DENIED
                                ? std::string("the pipe exists already: another helper (or a program posing as one) runs")
                                : "cannot create the pipe: " + windowsError(code);
                    ok = false;
                    break;
                }
                if (code != lastCreateError) {
                    WE_LOG_WARN(log(), "helper: cannot make another pipe instance: {} (trying again)",
                                windowsError(code));
                }
                lastCreateError = code;
                if (WaitForSingleObject(stop, 250) == WAIT_OBJECT_0) break;
                continue;
            }
            first = false;
            lastCreateError = ERROR_SUCCESS;
        }
        if (workers.size() >= static_cast<std::size_t>(kMaxConnections)) {
            // Full: wait for a connection to end (or the stop).
            const HANDLE waits[2] = {stop, freed};
            WaitForMultipleObjects(freed != nullptr ? 2 : 1, waits, FALSE, 1000);
            continue;
        }
        bool connected = false;
        {
            const IoEvent event;
            if (event.get() != nullptr) {
                OVERLAPPED overlapped{};
                overlapped.hEvent = event.get();
                DWORD unused = 0;
                const BOOL started = ConnectNamedPipe(listening, &overlapped);
                connected = (!started && GetLastError() == ERROR_PIPE_CONNECTED)   // the client was quicker
                            || finish(listening, overlapped, started, stop, INFINITE, unused);
            } else {
                WE_LOG_ERROR(log(), "helper: no event to wait for a caller: {}", windowsError(GetLastError()));
                WaitForSingleObject(stop, 250);
            }
        }
        if (!connected) {
            // Stopped, or the client gave up before it was served: this instance listens again.
            DisconnectNamedPipe(listening);
            continue;
        }
        const HANDLE pipe = listening;
        listening = INVALID_HANDLE_VALUE;   // the next turn makes the next one at once
        try {
            Worker& worker = workers.emplace_back();
            worker.thread = std::thread([pipe, stop, freed, this, &hellos, done = worker.done] {
                {
                    Connection connection(pipe, stop, options_, hellos);
                    connection.serve();
                }
                done->store(true);
                if (freed != nullptr) SetEvent(freed);
            });
        } catch (const std::exception& e) {
            // No thread for it (out of resources): this caller is turned away, the helper goes on.
            if (!workers.empty() && !workers.back().thread.joinable()) workers.pop_back();
            WE_LOG_ERROR(log(), "helper: cannot serve a caller: {}", e.what());
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }
    if (listening != INVALID_HANDLE_VALUE) CloseHandle(listening);
    for (Worker& worker : workers) worker.thread.join();
    if (freed != nullptr) CloseHandle(freed);
    LocalFree(descriptor);
    WE_LOG_INFO(log(), "helper: stopped listening");
    return ok;
}

}  // namespace wheelbri::helper
