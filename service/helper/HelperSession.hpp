#pragma once

#include "Privileged.hpp"

#include "wheelbri/HelperDispatch.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/HelperProtocol.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wheelbri::service {
class VirtualWheelDevice;
}

namespace wheelbri::helper {

// What one accepted connection has asked the helper for, and its undoing: the virtual G920 it created is removed,
// the rig is shown again (if this connection hid it) and the hide filter goes out of the interfaces this connection
// put it in (and did not take it out of) when the session ends, which is when the pipe closes (Stop,
// the app crashing, logoff) or the helper stops. One thread at a time (the connection's).
//
// Each request is checked by requestProblem before it gets here; this checks again what only Windows can tell (an
// interface is present and is not a keyboard, mouse, ...) and never takes a process, a path or a command from it:
// - HideRig names the caller's own process (`process`, opened by the server and named by its handle).
// - ShareLogFolder takes the caller's own %LOCALAPPDATA%\wheelbri\logs (from its token), opened as the caller.
// - SetLogLevel writes the one level it was given (checked: trace, debug, info or warn) to HKLM\SOFTWARE\wheelbri
//   LogLevel for the drivers; the server sets the helper's own log to it too (HelperDispatch: Answer::logLevel).
// - Driver scripts come only from the helper's own folder (`scriptLevels` 0 as a service; up to 3 with --console,
//   for a development build in the source tree).
class HelperSession final : public RequestHandler {
public:
    using Cancelled = std::function<bool()>;

    // The privileged steps a session takes (service::privileged and HideFilter::interfaceProblem by default):
    // HelperSessionTest puts fakes here, to check what a session does and that it undoes it.
    struct Steps {
        std::function<bool(void* process, std::string& message)> allowOnly;
        std::function<std::uint32_t()> allowedPid;
        std::function<bool(std::uint32_t pid, std::uint64_t processStart)> writeAllowedPid;
        std::function<std::string(const std::string& instanceId)> interfaceProblem;
        std::function<service::privileged::HideSetup(const std::vector<std::string>& install,
                                                     const std::vector<std::string>& interfaces, int levelsAbove,
                                                     const std::filesystem::path& logFolder,
                                                     const Cancelled& cancelled, std::string& message)>
            putHideFilter;
        std::function<service::privileged::HideSetup(const std::vector<std::string>& interfaces,
                                                     const Cancelled& cancelled, std::string& message)>
            takeHideFilterOff;
        std::function<bool(int levelsAbove, const std::filesystem::path& logFolder, const Cancelled& cancelled,
                           std::string& message)>
            installWheelDriver;
        std::function<bool(LogLevel level, std::string& message)> writeLogLevel;
    };
    [[nodiscard]] static Steps systemSteps();

    // `process`, `token`: HANDLEs this session keeps and closes (the caller's process: PROCESS_QUERY_LIMITED_INFORMATION
    // | SYNCHRONIZE | READ_CONTROL | WRITE_DAC; an impersonation token of the caller, or null when it gave none).
    HelperSession(void* process, void* token, int scriptLevels, Cancelled cancelled, Steps steps = systemSteps());
    ~HelperSession() override;

    HelperSession(const HelperSession&) = delete;
    HelperSession& operator=(const HelperSession&) = delete;

    // Carries out a request that passed requestProblem (not Hello).
    [[nodiscard]] Reply handle(const Request& request) override;

private:
    Reply createVirtualWheel(const Request& request);
    Reply hideRig(std::uint32_t seq);
    Reply showRig(std::uint32_t seq);
    Reply putHideFilter(const Request& request);
    Reply takeHideFilterOff(const Request& request);
    Reply installWheelDriver(std::uint32_t seq);
    Reply shareLogFolder(std::uint32_t seq);
    Reply setLogLevel(const Request& request);
    void showRigIfOurs();
    // Where the driver install scripts write their logs: the caller's logs folder once ShareLogFolder took it, else
    // ProgramData's (never SYSTEM's own %LOCALAPPDATA%, where nobody would look).
    [[nodiscard]] std::filesystem::path scriptLogFolder() const;

    void* process_;   // HANDLE
    void* token_;     // HANDLE or null
    std::uint32_t pid_ = 0;
    int scriptLevels_;
    Cancelled cancelled_;
    Steps steps_;
    std::unique_ptr<service::VirtualWheelDevice> device_;
    const VirtualWheel* deviceWheel_ = nullptr;   // which one device_ is
    bool hid_ = false;
    std::vector<std::string> filterOn_;   // interfaces PutHideFilter put the filter in, until TakeHideFilterOff
    std::filesystem::path logFolder_;   // the caller's, checked by shareLogFolder
};

}  // namespace wheelbri::helper
