// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Who may send the driver its datagrams on 127.0.0.1 (the wheel's state on 41329, TrueForce's on 41331): only
// WheelBri (wheelbri.exe, the app, or wheelbri-service.exe). Any program of any user can send to those ports, and what
// arrives there steers the virtual wheel games read, so the driver asks Windows which process owns the sending port
// and which program that is, as the service does for force feedback (SenderGate.hpp). Checked from WUDFHost as
// LocalService on Windows 11 (2026-11-01): the lookup names wheelbri.exe, an Administrator process included.
//
// - Another program, or a port no process owns any more: its datagrams are dropped, with one warning per sender.
// - Windows cannot say (a call failed): let through, with a warning; the stream must not be lost to a failed lookup.
//
// SenderCheck (the verdicts per sender) is portable and tested everywhere (tests/DriverSenderProbeTest.cpp);
// lookUpSender is the Windows part. iphlpapi is looked up at run time, so the driver links nothing new.

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>   // before windows.h
#include <windows.h>
#include <iphlpapi.h>
#endif

namespace wheelbri::driver {

enum class SenderVerdict {
    Accept,    // WheelBri
    Refuse,    // another program, or nobody owns the port any more
    Unknown,   // Windows could not say: let through
};

// What the lookup found, with the log line that says so.
struct SenderLookup {
    SenderVerdict verdict = SenderVerdict::Unknown;
    std::string line;
};

// The verdict per sender port, so the lookup runs only when a new sender appears, and again after a while (a port can
// be closed and taken by another program): a let-through sender after kAcceptRecheckMs, a refused one after
// kRefuseRecheckMs. At most kLookupsPerSecond lookups a second: a flood from ever new ports cannot keep the receiving
// thread busy; past the cap a new sender is dropped, and a known one keeps its last verdict. No allocation besides
// the log lines.
class SenderCheck {
public:
    static constexpr std::size_t kEntries = 16;
    static constexpr std::int64_t kAcceptRecheckMs = 60'000;
    static constexpr std::int64_t kRefuseRecheckMs = 10'000;
    static constexpr std::uint32_t kLookupsPerSecond = 8;

    // Whether a datagram from `port` at `nowMs` is let through. `lookUp(port)` gives a SenderLookup. `log` gets a line
    // to write when there is something new to say (a new sender, a changed verdict, a refusal or a failed lookup), and
    // `warn` whether it is a warning; `log` is left empty otherwise.
    template <class LookUp>
    [[nodiscard]] bool allow(const std::uint16_t port, const std::int64_t nowMs, LookUp&& lookUp, std::string& log,
                             bool& warn) {
        log.clear();
        warn = false;
        Entry* entry = find(port);
        if (entry != nullptr) entry->lastUseMs = nowMs;
        if (entry != nullptr && nowMs < entry->expiresMs) return entry->accept;
        if (!takeLookup(nowMs)) return entry != nullptr && entry->accept;
        SenderLookup found = std::forward<LookUp>(lookUp)(port);
        const bool accept = found.verdict != SenderVerdict::Refuse;
        const bool changed = entry == nullptr || entry->accept != accept;
        if (changed || found.verdict != SenderVerdict::Accept) {
            log = std::move(found.line);
            warn = found.verdict != SenderVerdict::Accept;
        }
        if (entry == nullptr) entry = &slotFor(port);
        entry->accept = accept;
        entry->expiresMs = nowMs + (accept ? kAcceptRecheckMs : kRefuseRecheckMs);
        entry->lastUseMs = nowMs;
        return accept;
    }

private:
    struct Entry {
        std::uint16_t port = 0;
        bool used = false;
        bool accept = false;
        std::int64_t expiresMs = 0;
        std::int64_t lastUseMs = 0;
    };

    Entry* find(const std::uint16_t port) {
        for (Entry& entry : entries_) {
            if (entry.used && entry.port == port) return &entry;
        }
        return nullptr;
    }

    // A free entry, or the one used longest ago, now for `port`.
    Entry& slotFor(const std::uint16_t port) {
        Entry* chosen = &entries_[0];
        for (Entry& entry : entries_) {
            if (!entry.used) {
                chosen = &entry;
                break;
            }
            if (entry.lastUseMs < chosen->lastUseMs) chosen = &entry;
        }
        *chosen = Entry{};
        chosen->port = port;
        chosen->used = true;
        return *chosen;
    }

    bool takeLookup(const std::int64_t nowMs) {
        if (nowMs - windowStartMs_ >= 1000) {
            windowStartMs_ = nowMs;
            lookups_ = 0;
        }
        if (lookups_ >= kLookupsPerSecond) return false;
        ++lookups_;
        return true;
    }

    std::array<Entry, kEntries> entries_{};
    std::int64_t windowStartMs_ = INT64_MIN / 2;
    std::uint32_t lookups_ = 0;
};

// Whether `image` (a full path) is WheelBri's own program: wheelbri.exe (the app) or wheelbri-service.exe, any case.
[[nodiscard]] inline bool isWheelBriProgram(const std::wstring& image) {
    const std::size_t slash = image.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? image : image.substr(slash + 1);
    for (wchar_t& c : name) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return name == L"wheelbri.exe" || name == L"wheelbri-service.exe";
}

#ifdef _WIN32

namespace detail {

inline std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return "?";
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// The process that owns IPv4 UDP port `port` (bound to 127.0.0.1 or any address); 0 = nobody. `error` gets the
// Windows error when the table cannot be read (then the result is 0 too).
inline DWORD udpPortOwner(const std::uint16_t port, DWORD& error) {
    using GetTable = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, UDP_TABLE_CLASS, ULONG);
    static const GetTable getTable = [] {
        const HMODULE module = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module == nullptr ? GetTable{nullptr}
                                 : reinterpret_cast<GetTable>(
                                       reinterpret_cast<void*>(GetProcAddress(module, "GetExtendedUdpTable")));
    }();
    error = 0;
    if (getTable == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return 0;
    }
    std::string buffer(16 * 1024, '\0');
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD result = getTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            buffer.assign(static_cast<std::size_t>(size) + 4096, '\0');   // sockets come and go: some room to spare
            continue;
        }
        if (result != NO_ERROR) {
            error = result;
            return 0;
        }
        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_UDPROW_OWNER_PID& row = table->table[i];
            const auto rowPort = ntohs(static_cast<u_short>(row.dwLocalPort));
            const bool loopbackOrAny = row.dwLocalAddr == htonl(INADDR_LOOPBACK) || row.dwLocalAddr == INADDR_ANY;
            if (rowPort == port && loopbackOrAny) return row.dwOwningPid;
        }
        return 0;
    }
    error = ERROR_INSUFFICIENT_BUFFER;
    return 0;
}

}  // namespace detail

// Who sends `what` from 127.0.0.1:`port`: the verdict, and a log line with the process id and program, or with the
// step Windows refused and its error.
[[nodiscard]] inline SenderLookup lookUpSender(const std::uint16_t port, const char* const what) {
    SenderLookup out;
    std::string& line = out.line;
    line = "sender check: ";
    line += what;
    line += " from port ";
    line += std::to_string(port);
    DWORD error = 0;
    const DWORD pid = detail::udpPortOwner(port, error);
    if (error != 0) {
        line += ": the UDP table cannot be read (error ";
        line += std::to_string(error);
        line += "); let through";
        out.verdict = SenderVerdict::Unknown;
        return out;
    }
    if (pid == 0) {
        line += ": no process owns that port any more; dropped";
        out.verdict = SenderVerdict::Refuse;
        return out;
    }
    line += ", pid ";
    line += std::to_string(pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        line += ": OpenProcess refused (error ";
        line += std::to_string(GetLastError());
        line += "); let through";
        out.verdict = SenderVerdict::Unknown;
        return out;
    }
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    const BOOL named = QueryFullProcessImageNameW(process, 0, image.data(), &size);
    const DWORD nameError = named ? 0 : GetLastError();
    CloseHandle(process);
    if (!named) {
        line += ": its program cannot be read (QueryFullProcessImageName error ";
        line += std::to_string(nameError);
        line += "); let through";
        out.verdict = SenderVerdict::Unknown;
        return out;
    }
    image.resize(size);
    line += ", ";
    line += detail::utf8(image);
    if (isWheelBriProgram(image)) {
        line += ": WheelBri";
        out.verdict = SenderVerdict::Accept;
    } else {
        line += ": not WheelBri; its datagrams are dropped";
        out.verdict = SenderVerdict::Refuse;
    }
    return out;
}

#endif

}  // namespace wheelbri::driver
