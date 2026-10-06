#pragma once

#include <string>

namespace wheelbri::helper {

// The helper's side of the pipe (\\.\pipe\wheelbri-helper, HelperProtocol.hpp).
//
// - Only processes on this computer, of the logged-on (interactive) user, SYSTEM or Administrators may open it (the
//   pipe's DACL; remote clients are rejected), and only this server may create it (first instance: a program that
//   created the name first makes run() fail, so nobody can pose as the helper).
// - A caller is accepted only when it is wheelbri.exe in the helper's own folder (`appFolder`): the process is
//   opened and named by its handle, and its id is compared with the pipe's again after that, so it cannot have been
//   swapped. Anything else may only say Hello (the reply says why it is refused), which is how `--hello` checks a
//   running helper.
// - Every request is checked by requestProblem before anything is done.
// - Up to kMaxConnections at once, each on its own thread with its HelperSession, which carries the requests out.
//   When a connection closes (Stop, a crash, logoff) what it set up is undone: its virtual G920 removed, the rig shown
//   again.
class HelperServer {
public:
    static constexpr int kMaxConnections = 4;

    // `scriptLevels`: how far above the helper's folder driver scripts may be found (privileged::findDriverScript):
    // 0 as a service, more only with --console for a development build.
    HelperServer(std::wstring appFolder, int scriptLevels);

    // Serves until `stopEvent` (a manual-reset event HANDLE) is set; every connection is closed before it returns.
    // False, with `error`, when the pipe cannot be created (e.g. another helper is running).
    bool run(void* stopEvent, std::string& error);

private:
    std::wstring appFolder_;
    int scriptLevels_;
};

}  // namespace wheelbri::helper
