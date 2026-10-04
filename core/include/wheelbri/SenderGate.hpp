#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace wheelbri {

// Who may send to the UDP ports on 127.0.0.1. Any program of the user can send a datagram there, so on Windows the
// receiver asks which process owns the sending port (once per new port, cached) and lets through only the programs
// that should be talking to it: game force feedback (port 41330) from the virtual G920's driver host or a game with
// WheelBri's shim, the wheel's state (41329) only from WheelBri. The lookup is the platform's (GetExtendedUdpTable on
// Windows); this file holds the rules and the cache, so they are tested everywhere.

// What the lookup found out about a sender port.
enum class SenderVerdict {
    Accept,    // a program allowed to send here
    Refuse,    // another program, or no program owns that port any more
    Unknown,   // the system could not say (an API failed; under Wine it often does): the gate's policy decides
};

// The virtual G920's driver host: <system folder>\WUDFHost.exe (any case).
[[nodiscard]] bool isDriverHostImage(const std::filesystem::path& image, const std::filesystem::path& systemFolder);
// WheelBri itself: wheelbri.exe or wheelbri-service.exe (any case, any folder).
[[nodiscard]] bool isWheelBriImage(const std::filesystem::path& image);
// The same folder, ignoring case and a trailing separator (Windows paths).
[[nodiscard]] bool sameFolder(const std::filesystem::path& a, const std::filesystem::path& b);

// Remembers the verdict per sender port for a while, so the lookup runs only when a new sender appears (and again
// after the time to live: a port can be closed and taken by another program). Lookups are capped per second, so a
// flood from ever new ports cannot keep the receiver busy: past the cap, new senders are refused until the next
// second. No allocation after construction.
class SenderGate {
public:
    using Lookup = std::function<SenderVerdict(std::uint16_t port)>;

    struct Settings {
        std::uint64_t acceptTtlNs = 2'000'000'000;     // a sender let through is looked up again after this
        std::uint64_t refuseTtlNs = 10'000'000'000;
        bool acceptUnknown = false;                    // the verdict when the lookup cannot tell
        std::uint32_t maxLookupsPerSecond = 16;
    };

    struct Stats {
        std::uint64_t lookups = 0;
        std::uint64_t refused = 0;          // datagrams dropped
        std::uint64_t unknown = 0;          // lookups that could not tell
        std::uint64_t overLookupCap = 0;    // datagrams dropped because the lookups for this second were used up
    };

    explicit SenderGate(Lookup lookup) : SenderGate(std::move(lookup), Settings{}) {}
    SenderGate(Lookup lookup, Settings settings);

    // Whether a datagram from `port` (the sender's UDP port on 127.0.0.1) at `nowNs` is let through.
    [[nodiscard]] bool allow(std::uint16_t port, std::uint64_t nowNs);

    // Whether the last allow() call ran the lookup and refused (for a log line per new refused sender).
    [[nodiscard]] bool lastWasNewRefusal() const { return lastWasNewRefusal_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }

    static constexpr std::size_t kEntries = 16;

private:
    struct Entry {
        std::uint16_t port = 0;
        bool used = false;
        bool accept = false;
        std::uint64_t expiresNs = 0;
        std::uint64_t lastUseNs = 0;
    };

    Lookup lookup_;
    Settings settings_;
    std::array<Entry, kEntries> entries_{};
    std::uint64_t windowStartNs_ = 0;
    std::uint32_t lookupsInWindow_ = 0;
    bool lastWasNewRefusal_ = false;
    Stats stats_;
};

}  // namespace wheelbri
