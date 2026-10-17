#pragma once

#include "wheelbri/LogLevel.hpp"
#include "wheelbri/StatePacket.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace wheelbri {

class IStatePublisher {
public:
    virtual ~IStatePublisher() = default;
    virtual bool publish(const StatePacket& packet) = 0;
};

class IStateReceiver {
public:
    virtual ~IStateReceiver() = default;
    // Drains every queued packet without blocking and returns the newest one, if any arrived.
    [[nodiscard]] virtual std::optional<StatePacket> pollLatest() = 0;
};

// UDP on 127.0.0.1 only; nothing is ever sent off the machine.
class UdpLoopbackPublisher final : public IStatePublisher {
public:
    // Returns nullptr (and logs to stderr) if the socket cannot be created.
    [[nodiscard]] static std::unique_ptr<UdpLoopbackPublisher> create(std::uint16_t port);
    ~UdpLoopbackPublisher() override;

    UdpLoopbackPublisher(const UdpLoopbackPublisher&) = delete;
    UdpLoopbackPublisher& operator=(const UdpLoopbackPublisher&) = delete;

    bool publish(const StatePacket& packet) override;

    // The app's log level to the same reader (LogLevelPacket.hpp), from the same socket: the shim checks each new
    // sender port once (SenderGate), so another socket would cost the game's input thread another lookup.
    bool publishLogLevel(LogLevel level);

private:
    struct Impl;
    explicit UdpLoopbackPublisher(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class UdpLoopbackReceiver final : public IStateReceiver {
public:
    [[nodiscard]] static std::unique_ptr<UdpLoopbackReceiver> create(std::uint16_t port);
    ~UdpLoopbackReceiver() override;

    UdpLoopbackReceiver(const UdpLoopbackReceiver&) = delete;
    UdpLoopbackReceiver& operator=(const UdpLoopbackReceiver&) = delete;

    // Reads at most kMaxPerPoll datagrams a call (the rest wait for the next call).
    [[nodiscard]] std::optional<StatePacket> pollLatest() override;
    static constexpr std::size_t kMaxPerPoll = 64;

    // What the last pollLatest() did: datagrams read, and whether kMaxPerPoll stopped it (then newer ones still wait
    // in the socket and the state handed over is older than the newest sent).
    struct PollInfo {
        std::size_t read = 0;
        bool capped = false;
    };
    [[nodiscard]] PollInfo lastPoll() const;

    // The log level the service sent last (LogLevelPacket.hpp), read by pollLatest along with the states; nullopt
    // when none came since the previous call.
    [[nodiscard]] std::optional<LogLevel> takeLogLevel();

    // Optional: datagrams whose sender (its local UDP port on 127.0.0.1) the filter refuses are dropped.
    using SenderFilter = std::function<bool(std::uint16_t senderPort)>;
    void setSenderFilter(SenderFilter filter);

    // The port it listens on: `port` from create(), or the one the system chose for create(0) (tests use that, so
    // that tests running side by side never share a port). 0 if it cannot tell.
    [[nodiscard]] std::uint16_t localPort() const;

    // The socket itself (a SOCKET on Windows, a file descriptor elsewhere), for waiting until a state arrives
    // (wheelbri-uhid's poll). Owned by this object.
    [[nodiscard]] std::intptr_t nativeSocket() const;

private:
    struct Impl;
    explicit UdpLoopbackReceiver(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Raw datagrams on 127.0.0.1, for other message types (force feedback, FfbPacket). Non-blocking.
class UdpLoopbackSender {
public:
    // Returns nullptr (and logs why) if the socket cannot be created.
    [[nodiscard]] static std::unique_ptr<UdpLoopbackSender> create(std::uint16_t port);
    ~UdpLoopbackSender();

    UdpLoopbackSender(const UdpLoopbackSender&) = delete;
    UdpLoopbackSender& operator=(const UdpLoopbackSender&) = delete;

    bool send(std::span<const std::uint8_t> bytes);

private:
    struct Impl;
    explicit UdpLoopbackSender(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class UdpLoopbackListener {
public:
    // Returns nullptr (and logs why) if 127.0.0.1:port cannot be bound.
    [[nodiscard]] static std::unique_ptr<UdpLoopbackListener> create(std::uint16_t port);
    ~UdpLoopbackListener();

    UdpLoopbackListener(const UdpLoopbackListener&) = delete;
    UdpLoopbackListener& operator=(const UdpLoopbackListener&) = delete;

    // One queued datagram into `buffer`; its size, or nullopt if nothing is waiting. `senderPort` (optional) gets the
    // sender's UDP port on 127.0.0.1, to tell who sent it.
    [[nodiscard]] std::optional<std::size_t> receive(std::span<std::uint8_t> buffer,
                                                     std::uint16_t* senderPort = nullptr);

    // The port it listens on (the system's choice for create(0), as UdpLoopbackReceiver::localPort).
    [[nodiscard]] std::uint16_t localPort() const;

    // The socket itself (a SOCKET on Windows, a file descriptor elsewhere), for waiting until a datagram arrives
    // (the service's event loop, platform::WaitSet). Owned by this object.
    [[nodiscard]] std::intptr_t nativeSocket() const;

private:
    struct Impl;
    explicit UdpLoopbackListener(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelbri
