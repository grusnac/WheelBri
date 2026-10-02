#pragma once

#include "wheelemu/StatePacket.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace wheelemu {

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

    [[nodiscard]] std::optional<StatePacket> pollLatest() override;

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

    // One queued datagram into `buffer`; its size, or nullopt if nothing is waiting.
    [[nodiscard]] std::optional<std::size_t> receive(std::span<std::uint8_t> buffer);

private:
    struct Impl;
    explicit UdpLoopbackListener(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelemu
