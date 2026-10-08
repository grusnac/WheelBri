// wheelbri-helper.exe: the WheelBri Helper, a Windows service (WheelBriHelper, running as SYSTEM) that does the few
// steps that need Administrator for the app, so the app can run as a standard user (docs/future-functionalities.md).
//
//   wheelbri-helper              run by the service manager (the installer registers it)
//   wheelbri-helper --console    the same in this console, until Ctrl+C (Administrator; for development)
//   wheelbri-helper --hello      ask a running helper who it is and whether this program may use it
//   wheelbri-helper --version

#include "HelperClient.hpp"
#include "HelperLog.hpp"
#include "HelperServer.hpp"
#include "HelperSession.hpp"
#include "HideFilter.hpp"

#include "wheelbri/Logging.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>

namespace {

using namespace wheelbri;

constexpr wchar_t kServiceName[] = L"WheelBriHelper";

logging::Logger& log() {
    static logging::Logger logger = logging::get("helper");
    return logger;
}

HANDLE gStop = nullptr;
SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
SERVICE_STATUS gStatus{};

// The folder of this program: the only place whose wheelbri.exe the helper serves.
std::wstring ownFolder() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    const std::size_t cut = path.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

void startLogging(const bool console) {
    logging::LogConfig config;
    config.console = console;
    config.periodicFlush = false;   // HelperLog writes each line itself
    logging::init(config, std::string());
    helper::HelperLog::instance().install();
    WE_LOG_INFO(log(), "WheelBri Helper {} starting ({})", WHEELBRI_VERSION, console ? "console" : "service");
}

// `console`: started by hand (a development build may then find its driver scripts in the source tree above it).
int serve(const bool console) {
    // A rig left hidden for a process that is gone (the helper or the app crashed): visible again.
    service::HideFilter::clearStale();
    // Driver scripts only next to the helper as a service; a development build (--console) also 3 folders up.
    const int scriptLevels = console ? 3 : 0;
    helper::HelperServer::Options options;
    options.appFolder = ownFolder();
    options.version = std::string("WheelBri Helper ") + WHEELBRI_VERSION;
    options.makeSession = [scriptLevels](void* const process, void* const token, std::function<bool()> cancelled) {
        return std::make_unique<helper::HelperSession>(process, token, scriptLevels, std::move(cancelled));
    };
    options.takeLog = [](void* const token, const std::wstring& folder) {
        helper::HelperLog::instance().attachUser(token, folder);
    };
    helper::HelperServer server(std::move(options));
    std::string error;
    bool served = false;
    try {
        served = server.run(gStop, error);
    } catch (const std::exception& e) {
        error = std::string("stopped by an error: ") + e.what();
    } catch (...) {
        error = "stopped by an unknown error";
    }
    if (!served) {
        WE_LOG_CRITICAL(log(), "helper: {}", error);
        return 1;
    }
    WE_LOG_INFO(log(), "WheelBri Helper stopped");
    return 0;
}

void reportStatus(const DWORD state, const DWORD exitCode = NO_ERROR) {
    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = state;
    gStatus.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    gStatus.dwWin32ExitCode = exitCode == NO_ERROR ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    gStatus.dwServiceSpecificExitCode = exitCode;
    gStatus.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 5000;
    SetServiceStatus(gStatusHandle, &gStatus);
}

DWORD WINAPI serviceControl(const DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            reportStatus(SERVICE_STOP_PENDING);
            SetEvent(gStop);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    gStatusHandle = RegisterServiceCtrlHandlerExW(kServiceName, serviceControl, nullptr);
    if (gStatusHandle == nullptr) return;
    reportStatus(SERVICE_START_PENDING);
    startLogging(false);
    reportStatus(SERVICE_RUNNING);
    const int code = serve(false);
    logging::shutdown();
    reportStatus(SERVICE_STOPPED, static_cast<DWORD>(code));
}

BOOL WINAPI consoleControl(const DWORD) {
    SetEvent(gStop);
    return TRUE;
}

int hello() {
    helper::HelperClient client;
    std::string error;
    const helper::HelperClient::Connect connected = client.connect(error);
    if (connected != helper::HelperClient::Connect::Ok) {
        std::printf("%s\n", error.c_str());
        return connected == helper::HelperClient::Connect::NoHelper ? 3 : 1;
    }
    std::printf("helper: pid %lu, %s\n", static_cast<unsigned long>(client.serverPid()),
                client.serverImage().empty() ? "(program not readable)" : client.serverImage().c_str());
    const auto reply = client.call(helper::RequestType::Hello, {}, 5000, error);
    if (!reply) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %s\n", helper::statusName(reply->status), reply->text.c_str());
    return reply->status == helper::Status::Ok ? 0 : 2;
}

void usage() {
    std::printf("usage: wheelbri-helper [--console | --hello | --version]\n"
                "  (no option)  run as the WheelBriHelper service (started by Windows)\n"
                "  --console    run in this console until Ctrl+C (needs Administrator)\n"
                "  --hello      ask the running helper who it is; exit 0 accepted, 2 refused, 3 no helper, 1 error\n");
}

}  // namespace

int main(const int argc, char** argv) {
    const std::string option = argc > 1 ? argv[1] : "";
    if (argc > 2 || (!option.empty() && option != "--console" && option != "--hello" && option != "--version")) {
        usage();
        return 1;
    }
    if (option == "--version") {
        std::printf("WheelBri Helper %s\n", WHEELBRI_VERSION);
        return 0;
    }
    if (option == "--hello") return hello();

    gStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (option == "--console") {
        if (!isElevated()) {
            std::printf("--console needs Administrator (the helper does what the app may not)\n");
            return 1;
        }
        startLogging(true);
        SetConsoleCtrlHandler(consoleControl, TRUE);
        const int code = serve(true);
        logging::shutdown();
        return code;
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::printf("This is the WheelBri Helper service; Windows starts it. To try it by hand:\n");
            usage();
        } else {
            std::printf("cannot start as a service (error %lu)\n", GetLastError());
        }
        return 1;
    }
    return 0;
}
