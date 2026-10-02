#include "HidHide.hpp"

#include "wheelemu/HidHideCli.hpp"
#include "wheelemu/Logging.hpp"

#ifdef _WIN32

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace wheelemu::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.hidhide");
    return logger;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string join(const std::vector<std::string>& items) {
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) text += ", ";
        text += item;
    }
    return text.empty() ? "nothing" : text;
}

std::wstring thisProgram() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

struct HidHide::Impl {
    std::wstring cli;
    std::vector<std::string> hiddenByUs;
    bool cloakWasOff = false;

    // Runs HidHideCLI with `args`; exit code and its standard output (UTF-8 or UTF-16, both read).
    std::pair<DWORD, std::string> run(std::initializer_list<std::string> args) {
        std::wstring command = L"\"" + cli + L"\"";
        std::string shown;
        for (const auto& arg : args) {
            // Quoted only when it has to be, as .NET does for the bridge: HidHideCLI reads options that arrive
            // in quotes ("--dev-gaming") as something else and answers with almost nothing.
            const bool quote = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
            command += L" " + (quote ? L"\"" + widen(arg) + L"\"" : widen(arg));
            shown += (shown.empty() ? "" : " ") + arg;
        }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return {~0UL, {}};
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writeEnd;
        si.hStdError = writeEnd;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &si, &pi);
        CloseHandle(writeEnd);
        if (!started) {
            CloseHandle(readEnd);
            WE_LOG_WARN(log(), "hidhide: cannot start {} (error {})", narrow(cli), GetLastError());
            return {~0UL, {}};
        }
        std::string bytes;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) bytes.append(buffer, got);
        CloseHandle(readEnd);
        WaitForSingleObject(pi.hProcess, 10'000);
        DWORD exit = ~0UL;
        GetExitCodeProcess(pi.hProcess, &exit);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // UTF-16 output has a zero byte after every ASCII character.
        std::size_t zeros = 0;
        for (const char c : bytes) zeros += c == '\0' ? 1 : 0;
        std::string output = bytes;
        if (bytes.size() >= 4 && zeros * 3 > bytes.size()) {
            std::wstring wide(bytes.size() / 2, L'\0');
            std::memcpy(wide.data(), bytes.data(), wide.size() * 2);
            if (!wide.empty() && wide[0] == 0xFEFF) wide.erase(0, 1);
            output = narrow(wide);
        }
        if (exit != 0) {
            WE_LOG_WARN(log(), "hidhide: {} -> exit code {} {}", shown, exit, output.substr(0, 200));
        }
        return {exit, output};
    }

    std::vector<std::string> hiddenList() { return hidhide::quotedValues(run({"--dev-list"}).second); }

    // Unhides `paths` (several tries, HidHide wants its own spelling); the ones still hidden.
    std::vector<std::string> unhide(const std::vector<std::string>& paths) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto still = hidhide::storedSpellings(hiddenList(), paths);
            if (still.empty()) return still;
            for (const auto& path : still) run({"--dev-unhide", path});
        }
        return hidhide::storedSpellings(hiddenList(), paths);
    }
};

std::unique_ptr<HidHide> HidHide::find() {
    std::vector<std::filesystem::path> candidates;
    for (const wchar_t* variable : {L"ProgramW6432", L"ProgramFiles"}) {
        wchar_t root[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(variable, root, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        const std::filesystem::path folder = std::filesystem::path(root) / L"Nefarius Software Solutions" / L"HidHide";
        candidates.push_back(folder / L"HidHideCLI.exe");
        candidates.push_back(folder / L"x64" / L"HidHideCLI.exe");
    }
    for (const auto& path : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            auto impl = std::make_unique<Impl>();
            impl->cli = path.wstring();
            return std::unique_ptr<HidHide>(new HidHide(std::move(impl)));
        }
    }
    return nullptr;
}

bool HidHide::hideWheel(const std::string& vendorId) {
    Impl& h = *impl_;
    const std::string self = narrow(thisProgram());
    const auto apps = hidhide::quotedValues(h.run({"--app-list"}).second);
    bool allowed = false;
    for (const auto& app : apps) allowed = allowed || hidhide::equalsIgnoreCase(app, self);
    if (!allowed) {
        h.run({"--app-reg", self});
        WE_LOG_INFO(log(), "hidhide: {} may still see hidden devices", self);
    }

    const auto [exit, json] = h.run({"--dev-gaming"});
    const auto wheel = exit == 0 ? hidhide::gamingDevicesOf(json, vendorId) : std::vector<std::string>{};
    if (wheel.empty()) {
        WE_LOG_WARN(log(), "hidhide: no game controller with VID {} found; games see the real wheel too", vendorId);
        WE_LOG_WARN(log(), "hidhide: --dev-gaming exit {}, {} bytes ({}); VID {} devices: {}", exit, json.size(),
                    json.substr(0, 160), vendorId, join(hidhide::describeDevicesOf(json, vendorId)));
        return false;
    }
    const auto alreadyHidden = h.hiddenList();
    for (const auto& path : wheel) {
        if (!hidhide::storedSpellings(alreadyHidden, {path}).empty()) continue;
        if (h.run({"--dev-hide", path}).first == 0) h.hiddenByUs.push_back(path);
    }
    h.cloakWasOff = h.run({"--cloak-state"}).second.find("--cloak-off") != std::string::npos;
    h.run({"--cloak-on"});
    WE_LOG_INFO(log(), "hidhide: real wheel hidden from games ({})", join(wheel));
    logState();
    return true;
}

void HidHide::restore() {
    Impl& h = *impl_;
    if (!h.hiddenByUs.empty()) {
        const auto still = h.unhide(h.hiddenByUs);
        if (still.empty()) {
            WE_LOG_INFO(log(), "hidhide: real wheel visible again");
        } else {
            WE_LOG_ERROR(log(), "hidhide: FAILED to unhide {}; run: wheelemu-service --unhide", join(still));
        }
    }
    if (h.cloakWasOff) {
        h.run({"--cloak-off"});
        WE_LOG_INFO(log(), "hidhide: hiding switched off again (it was off before)");
    }
    h.hiddenByUs.clear();
    h.cloakWasOff = false;
}

void HidHide::unhideAll(const std::string& vendorId) {
    Impl& h = *impl_;
    std::vector<std::string> ours;
    for (const auto& path : h.hiddenList()) {
        if (hidhide::containsIgnoreCase(path, "VID_" + vendorId)) ours.push_back(path);
    }
    const auto still = h.unhide(ours);
    if (still.empty()) {
        WE_LOG_INFO(log(), "hidhide: {} device(s) of VID {} visible again", ours.size(), vendorId);
    } else {
        WE_LOG_ERROR(log(), "hidhide: FAILED to unhide {}", join(still));
    }
}

void HidHide::logState() {
    Impl& h = *impl_;
    std::string cloak = h.run({"--cloak-state"}).second;
    while (!cloak.empty() && (cloak.back() == '\n' || cloak.back() == '\r' || cloak.back() == ' ')) cloak.pop_back();
    WE_LOG_INFO(log(), "hidhide: {}; hidden: {}", cloak, join(h.hiddenList()));
}

HidHide::HidHide(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HidHide::~HidHide() = default;

}  // namespace wheelemu::service

#else  // not Windows: nothing to hide with

namespace wheelemu::service {

struct HidHide::Impl {};

std::unique_ptr<HidHide> HidHide::find() { return nullptr; }
bool HidHide::hideWheel(const std::string&) { return false; }
void HidHide::restore() {}
void HidHide::unhideAll(const std::string&) {}
void HidHide::logState() {}
HidHide::HidHide(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HidHide::~HidHide() = default;

}  // namespace wheelemu::service

#endif
