#pragma once

#include "wheelbri/HelperDispatch.hpp"

#include <functional>
#include <memory>
#include <string>

namespace wheelbri::helper {

// The helper's side of the pipe (\\.\pipe\wheelbri-helper, HelperProtocol.hpp).
//
// - Only processes on this computer, of the logged-on (interactive) user, SYSTEM or Administrators may open it (the
//   pipe's DACL; remote clients are rejected), and only this server may create it (first instance: a program that
//   created the name first makes run() fail, so nobody can pose as the helper).
// - A caller is accepted only when it is wheelbri.exe or wheelbri-service.exe in the helper's own folder
//   (`appFolder`): the process is opened and named by its handle, and its id is compared with the pipe's again after
//   that, so it cannot have been swapped. Anything else may only say Hello (the reply says why it is refused, and the
//   connection is closed), which is how `--hello` checks a running helper.
// - Every message is answered by answerMessage (HelperDispatch.hpp): its rules, and a session that throws is a
//   failed request, not a helper that stops.
// - A connection must send its first message within `firstMessageMs`, so a program that connects and says nothing
//   cannot hold one of the kMaxConnections; each is served on its own thread. One more pipe instance always waits
//   unserved, so the pipe's name never goes away while the helper runs (callers find it busy, never missing).
// - When a connection closes (Stop, a crash, logoff) its session is destroyed, which undoes what it set up: its
//   virtual G920 removed, the rig shown again.
// - The helper's log follows WheelBri (mayTakeTheLog): the first message of an accepted caller, or a Hello from
//   WheelBri in another folder, moves it into that user's own log folder, opened as that user (HelperLog.hpp).
class HelperServer {
public:
    static constexpr int kMaxConnections = 4;
    static constexpr int kFirstMessageMs = 10'000;
    static constexpr int kWriteMs = 10'000;

    // An accepted caller's session. `process` (the caller's, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE |
    // READ_CONTROL | WRITE_DAC) and `token` (an impersonation token of the caller, or null) are HANDLEs the factory
    // takes, also when it returns null or throws. `cancelled`: true once the helper stops or the caller has gone.
    using SessionFactory =
        std::function<std::unique_ptr<RequestHandler>(void* process, void* token, std::function<bool()> cancelled)>;
    // Moves the helper's log into `logsFolder` (UTF-16), opened as the user of `token`, a HANDLE it takes.
    using LogTaker = std::function<void(void* token, const std::wstring& logsFolder)>;

    struct Options {
        std::wstring appFolder;        // the helper's own folder: only WheelBri there is accepted
        std::string version;           // "WheelBri Helper 0.9.0", the Hello reply
        std::wstring pipeName;         // empty: kPipeName (the tests use their own)
        SessionFactory makeSession;    // empty: no sessions (every request but Hello fails)
        LogTaker takeLog;              // empty: the log stays where it is
        // Only the tests: which caller programs count as WheelBri (empty: isInstalledApp in appFolder).
        std::function<bool(const std::string& image)> isApp;
        int firstMessageMs = kFirstMessageMs;
    };

    explicit HelperServer(Options options);

    // Serves until `stopEvent` (a manual-reset event HANDLE) is set; every connection is closed before it returns.
    // False, with `error`, when the pipe cannot be created (e.g. another helper is running).
    bool run(void* stopEvent, std::string& error);

private:
    Options options_;
};

}  // namespace wheelbri::helper
