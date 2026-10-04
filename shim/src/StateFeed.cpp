#include "StateFeed.hpp"

#include "ShimConfig.hpp"
#include "ShimLogging.hpp"

#include "wheelbri/FileText.hpp"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <string>
#include <vector>

namespace wheelbri::shim {

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.feed");
    return instance;
}

using GetExtendedUdpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, UDP_TABLE_CLASS, ULONG);

// iphlpapi.dll from System32, loaded on first use (never in DllMain; the shim links nothing at load time).
GetExtendedUdpTableFn udpTableFunction() {
    static const GetExtendedUdpTableFn function = [] {
        const HMODULE module = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module == nullptr ? nullptr
                                 : reinterpret_cast<GetExtendedUdpTableFn>(
                                       reinterpret_cast<void*>(GetProcAddress(module, "GetExtendedUdpTable")));
    }();
    return function;
}

// The process owning IPv4 UDP `port`, 0 if none; nullopt when the system cannot say.
std::optional<DWORD> udpPortOwner(const std::uint16_t port) {
    const GetExtendedUdpTableFn table = udpTableFunction();
    if (table == nullptr) return std::nullopt;
    std::vector<std::uint8_t> buffer;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD status = table(buffer.empty() ? nullptr : buffer.data(), &size, FALSE, AF_INET,
                                   UDP_TABLE_OWNER_PID, 0);
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            buffer.resize(static_cast<std::size_t>(size) + 16 * sizeof(MIB_UDPROW_OWNER_PID));
            continue;
        }
        if (status != NO_ERROR || buffer.empty()) return std::nullopt;
        const auto* rows = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
        const auto wanted = static_cast<std::uint16_t>((port >> 8) | (port << 8));   // network byte order
        for (DWORD i = 0; i < rows->dwNumEntries; ++i) {
            if (static_cast<std::uint16_t>(rows->table[i].dwLocalPort & 0xFFFFU) == wanted) {
                return rows->table[i].dwOwningPid;
            }
        }
        return 0UL;
    }
    return std::nullopt;
}

// Who sent the wheel's state. Refused only when the sender is known to be another program: under Proton the service
// is a Linux program Wine cannot name, and a game may not be allowed to look into WheelBri (it runs as
// Administrator), so "cannot tell" lets it through.
SenderVerdict lookupStateSender(const std::uint16_t port) {
    const std::optional<DWORD> pid = udpPortOwner(port);
    if (!pid || *pid == 0) return SenderVerdict::Unknown;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid);
    if (process == nullptr) return SenderVerdict::Unknown;
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    CloseHandle(process);
    if (ok == FALSE) return SenderVerdict::Unknown;
    path.resize(size);
    const std::filesystem::path image(path);
    if (isWheelBriImage(image)) return SenderVerdict::Accept;
    static logging::RateLimiter limiter(std::chrono::seconds(30));
    std::uint64_t suppressed = 0;
    if (limiter.allow(suppressed)) {
        WE_LOG_WARN(log(), "ignoring wheel state from {} (pid {}): only WheelBri may send it ({} similar suppressed)",
                    pathToUtf8(image), *pid, suppressed);
    }
    return SenderVerdict::Refuse;
}

SenderGate::Settings gateSettings() {
    SenderGate::Settings settings;
    settings.acceptUnknown = true;
    return settings;
}

std::uint64_t steadyNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

StateFeed::StateFeed() : gate_(lookupStateSender, gateSettings()) {}

void StateFeed::openReceiver(const std::uint16_t port) {
    receiver_ = UdpLoopbackReceiver::create(port);
    if (receiver_ != nullptr) {
        // Called inside pollLatest, under mutex_ (current() holds it).
        receiver_->setSenderFilter([this](const std::uint16_t sender) { return gate_.allow(sender, steadyNs()); });
    }
}

StateFeed& StateFeed::instance() {
    // Never destroyed: a static's destructor would run in DllMain (process detach) under the loader lock, and closing
    // its socket calls WSACleanup there, which can hang the game on exit.
    static StateFeed* const feed = new StateFeed;
    return *feed;
}

TargetReport StateFeed::current(const IDeviceProfile& profile) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const ShimConfig& config = shimConfig();

    if (!opened_) {
        opened_ = true;
        firstRequest_ = now;
        lastOpenAttempt_ = now;
        openReceiver(config.statePort);
        if (receiver_ == nullptr) {
            WE_LOG_ERROR(log(), "cannot listen on 127.0.0.1:{} (another program has it?); the emulated wheel stays "
                                "centred, trying again every 5 s",
                         config.statePort);
        }
    } else if (receiver_ == nullptr && now - lastOpenAttempt_ >= std::chrono::seconds(5)) {
        // Taken by a launcher that loaded this DLL first, or still held by an earlier run: it may free up.
        lastOpenAttempt_ = now;
        openReceiver(config.statePort);
        if (receiver_ != nullptr) {
            WE_LOG_INFO(log(), "listening on 127.0.0.1:{} now", config.statePort);
        }
    }

    if (receiver_ != nullptr) {
        if (auto packet = receiver_->pollLatest()) {
            latest_ = *packet;
            lastArrival_ = now;
            if (stale_) {
                stale_ = false;
                WE_LOG_INFO(log(), "receiving wheel state from wheelbri-service (packet #{})", packet->sequence);
            }
        }
    }

    const auto staleAfter = std::chrono::milliseconds(config.staleAfterMs);
    if (latest_ && now - lastArrival_ <= staleAfter) {
        return latest_->report;
    }

    if (!stale_) {
        stale_ = true;
        WE_LOG_WARN(log(), "no wheel state from wheelbri-service for {} ms: reporting a centred wheel and released "
                           "pedals until it comes back",
                    config.staleAfterMs);
    } else if (!latest_ && !warnedNeverReceived_ && now - firstRequest_ > std::chrono::seconds(2)) {
        warnedNeverReceived_ = true;
        WE_LOG_WARN(log(), "nothing received on port {} yet: is wheelbri-service running with the same state_port?",
                    config.statePort);
    }
    return profile.encode(WheelState{});
}

}  // namespace wheelbri::shim
