#include "ModuleWatch.hpp"

#include "DiFormat.hpp"
#include "ShimLogging.hpp"

#include "wheelbri/AxisAudit.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wheelbri::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.audit");
    return instance;
}

struct Module {
    std::string name;   // file name
    std::string path;
};

// Every module of this process; empty when the snapshot fails (logged by the caller).
std::vector<Module> loadedModules(DWORD& error) {
    std::vector<Module> modules;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    // ERROR_BAD_LENGTH: a module was loading at that moment; Microsoft says to try again.
    for (int attempt = 0; attempt < 5 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
        error = snapshot == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (error != ERROR_BAD_LENGTH) break;
    }
    if (snapshot == INVALID_HANDLE_VALUE) {
        return modules;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more != FALSE; more = Module32NextW(snapshot, &entry)) {
        modules.push_back(Module{toUtf8(entry.szModule, MAX_MODULE_NAME32 + 1), toUtf8(entry.szExePath)});
    }
    CloseHandle(snapshot);
    return modules;
}

std::string lowered(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

}  // namespace

void logNewModules(const std::string_view when) {
    static std::mutex mutex;
    static std::set<std::string> seen;   // lower-case paths
    const std::lock_guard<std::mutex> lock(mutex);

    DWORD error = 0;
    const std::vector<Module> modules = loadedModules(error);
    if (modules.empty()) {
        WE_LOG_WARN(log(), "modules at {}: could not list them (error {})", when, error);
        return;
    }
    const bool first = seen.empty();
    std::string input;
    std::string added;
    std::size_t newCount = 0;
    for (const Module& module : modules) {
        if (!seen.insert(lowered(module.path)).second) continue;
        ++newCount;
        const bool isInput = isInputModule(module.name);
        if (isInput) {
            input += (input.empty() ? "" : ", ") + module.path;
        }
        if (first) {
            WE_LOG_DEBUG(log(), "  module {}{}", module.path, isInput ? " [input]" : "");
        } else {
            added += (added.empty() ? "" : ", ") + module.path + (isInput ? " [input]" : "");
        }
    }
    if (first) {
        WE_LOG_INFO(log(),
                    "modules at {}: {} loaded; input-related: {} (hid.dll also comes with DirectInput itself; the "
                    "full list is at debug)",
                    when, modules.size(), input.empty() ? std::string("none") : input);
    } else if (newCount != 0) {
        WE_LOG_INFO(log(), "modules at {}: {} new since the last look: {}", when, newCount, added);
    } else {
        WE_LOG_DEBUG(log(), "modules at {}: none new ({} loaded)", when, modules.size());
    }
}

}  // namespace wheelbri::shim
