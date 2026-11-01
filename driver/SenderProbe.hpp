#pragma once

// Who sends the driver its datagrams on 127.0.0.1 (the wheel's state on 41329, TrueForce's on 41331): the first step
// towards letting only wheelbri-service through. Any program can send to those ports, so the driver will check the
// sender as the service already does for force feedback (SenderGate.hpp). Whether it can, running as LocalService in
// WUDFHost, is what this finds out: for each new sender port it looks up the owning process and its program, and
// says what it found, or which call Windows refused. It only logs; nothing is dropped.
//
// SenderSeen (the cache) is portable and tested everywhere (tests/DriverSenderProbeTest.cpp); lookUpSender is the
// Windows part. iphlpapi is looked up at run time, so the driver links nothing new.

#include <array>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>   // before windows.h
#include <windows.h>
#include <iphlpapi.h>
#endif

namespace wheelbri::driver {

// The sender ports already described, so the lookup runs once per new sender (and again after `kRecheckMs`: a port
// can be closed and taken by another program). At most `kLookupsPerSecond` lookups a second: a flood from ever new
// ports cannot keep the receiving thread busy. No allocation.
class SenderSeen {
public:
    static constexpr std::size_t kEntries = 16;
    static constexpr std::int64_t kRecheckMs = 60'000;
    static constexpr std::uint32_t kLookupsPerSecond = 8;

    // Whether a datagram from `port` at `nowMs` should be looked up now (and is remembered as looked up).
    [[nodiscard]] bool shouldLookUp(const std::uint16_t port, const std::int64_t nowMs) {
        Entry* oldest = &entries_[0];
        for (Entry& entry : entries_) {
            if (entry.used && entry.port == port) {
                if (nowMs - entry.seenMs < kRecheckMs) return false;
                if (!takeLookup(nowMs)) return false;
                entry.seenMs = nowMs;
                return true;
            }
            if (!entry.used || entry.seenMs < oldest->seenMs) oldest = &entry;
            if (!entry.used) break;
        }
        if (!takeLookup(nowMs)) return false;
        *oldest = Entry{port, true, nowMs};
        return true;
    }

private:
    struct Entry {
        std::uint16_t port = 0;
        bool used = false;
        std::int64_t seenMs = 0;
    };

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

// One log line on the sender of a datagram from 127.0.0.1:`port`: its process id, its program, and whether the
// planned check would let it through; or which step Windows refused, with its error.
[[nodiscard]] inline std::string lookUpSender(const std::uint16_t port, const char* const what) {
    std::string line = "sender check (log only, nothing dropped): ";
    line += what;
    line += " from port ";
    line += std::to_string(port);
    DWORD error = 0;
    const DWORD pid = detail::udpPortOwner(port, error);
    if (error != 0) {
        line += ": the UDP table cannot be read (error ";
        line += std::to_string(error);
        line += "); the check could not tell who sends";
        return line;
    }
    if (pid == 0) {
        line += ": no process owns that port any more; the check would refuse it";
        return line;
    }
    line += ", pid ";
    line += std::to_string(pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        line += ": OpenProcess refused (error ";
        line += std::to_string(GetLastError());
        line += "); the check could not tell which program it is";
        return line;
    }
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    const BOOL named = QueryFullProcessImageNameW(process, 0, image.data(), &size);
    const DWORD nameError = named ? 0 : GetLastError();
    CloseHandle(process);
    if (!named) {
        line += ": its program cannot be read (QueryFullProcessImageName error ";
        line += std::to_string(nameError);
        line += ")";
        return line;
    }
    image.resize(size);
    line += ", ";
    line += detail::utf8(image);
    line += isWheelBriProgram(image) ? ": WheelBri, the check would let it through"
                                     : ": not WheelBri, the check would refuse it";
    return line;
}

#endif

}  // namespace wheelbri::driver
