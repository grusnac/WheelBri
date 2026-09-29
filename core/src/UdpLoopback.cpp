#include "wheelemu/UdpLoopback.hpp"

#include "wheelemu/Logging.hpp"

#include <system_error>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wheelemu {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data;
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (ok_) {
            WSACleanup();
        }
    }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

void closeSocket(const NativeSocket socket) { closesocket(socket); }

bool makeNonBlocking(const NativeSocket socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

int lastError() { return WSAGetLastError(); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

class WinsockSession {
public:
    [[nodiscard]] bool ok() const { return true; }
};

void closeSocket(const NativeSocket socket) { ::close(socket); }

bool makeNonBlocking(const NativeSocket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

int lastError() { return errno; }
#endif

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("core.net");
    return logger;
}

std::string errorText(const int error) { return std::to_string(error) + " " + std::system_category().message(error); }

sockaddr_in loopbackAddress(const std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

// Owns one UDP socket plus the Winsock session it needs on Windows.
class UdpSocket {
public:
    UdpSocket() : socket_(session_.ok() ? ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : kInvalidSocket) {}
    ~UdpSocket() {
        if (socket_ != kInvalidSocket) {
            closeSocket(socket_);
        }
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    [[nodiscard]] bool valid() const { return socket_ != kInvalidSocket; }
    [[nodiscard]] NativeSocket get() const { return socket_; }

private:
    WinsockSession session_;
    NativeSocket socket_;
};

}  // namespace

struct UdpLoopbackPublisher::Impl {
    UdpSocket socket;
    sockaddr_in destination{};
};

UdpLoopbackPublisher::UdpLoopbackPublisher(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
UdpLoopbackPublisher::~UdpLoopbackPublisher() = default;

std::unique_ptr<UdpLoopbackPublisher> UdpLoopbackPublisher::create(const std::uint16_t port) {
    auto impl = std::make_unique<Impl>();
    if (!impl->socket.valid() || !makeNonBlocking(impl->socket.get())) {
        WE_LOG_ERROR(log(), "publisher: cannot create UDP socket: {}", errorText(lastError()));
        return nullptr;
    }
    impl->destination = loopbackAddress(port);
    WE_LOG_INFO(log(), "publisher ready, sending state to 127.0.0.1:{}", port);
    return std::unique_ptr<UdpLoopbackPublisher>(new UdpLoopbackPublisher(std::move(impl)));
}

bool UdpLoopbackPublisher::publish(const StatePacket& packet) {
    const auto bytes = encode(packet);
    const auto sent = ::sendto(impl_->socket.get(), reinterpret_cast<const char*>(bytes.data()),
                               static_cast<int>(bytes.size()), 0,
                               reinterpret_cast<const sockaddr*>(&impl_->destination), sizeof(impl_->destination));
    // No receiver yet (game not running) is normal: the datagram is simply dropped by the OS.
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        static logging::RateLimiter limiter(std::chrono::seconds(5));
        WE_LOG_WARN_LIMITED(log(), limiter, "publisher: sendto failed for packet #{}: {}", packet.sequence,
                            errorText(lastError()));
        return false;
    }
    return true;
}

struct UdpLoopbackReceiver::Impl {
    UdpSocket socket;
    std::uint16_t port = 0;
    std::optional<std::uint32_t> lastSequence;
    std::uint64_t received = 0;
    std::uint64_t lost = 0;
    logging::RateLimiter malformedLimiter{std::chrono::seconds(5)};
    logging::RateLimiter lossLimiter{std::chrono::seconds(5)};

    void track(const StatePacket& packet) {
        ++received;
        if (!lastSequence) {
            WE_LOG_INFO(log(), "receiver: first packet from service (#{}) on port {}", packet.sequence, port);
        } else if (packet.sequence <= *lastSequence) {
            // The service restarted and began counting from 1 again.
            WE_LOG_INFO(log(), "receiver: sequence restarted ({} -> {}), service was probably restarted",
                        *lastSequence, packet.sequence);
        } else if (packet.sequence != *lastSequence + 1) {
            const std::uint32_t gap = packet.sequence - *lastSequence - 1;
            lost += gap;
            logLoss(gap, packet.sequence);
        }
        lastSequence = packet.sequence;
    }

    void logLoss(const std::uint32_t gap, const std::uint32_t sequence) {
        std::uint64_t suppressed = 0;
        if (lossLimiter.allow(suppressed)) {
            WE_LOG_DEBUG(log(), "receiver: {} packets lost before #{} (total lost {} of {}, {} gaps not shown)", gap,
                         sequence, lost, received + lost, suppressed);
        }
    }
};

UdpLoopbackReceiver::UdpLoopbackReceiver(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
UdpLoopbackReceiver::~UdpLoopbackReceiver() = default;

std::unique_ptr<UdpLoopbackReceiver> UdpLoopbackReceiver::create(const std::uint16_t port) {
    auto impl = std::make_unique<Impl>();
    if (!impl->socket.valid() || !makeNonBlocking(impl->socket.get())) {
        WE_LOG_ERROR(log(), "receiver: cannot create UDP socket: {}", errorText(lastError()));
        return nullptr;
    }
    impl->port = port;
    const sockaddr_in address = loopbackAddress(port);
    if (::bind(impl->socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        WE_LOG_ERROR(log(), "receiver: cannot bind 127.0.0.1:{} (is another game instance running?): {}", port,
                     errorText(lastError()));
        return nullptr;
    }
    WE_LOG_INFO(log(), "receiver listening on 127.0.0.1:{}", port);
    return std::unique_ptr<UdpLoopbackReceiver>(new UdpLoopbackReceiver(std::move(impl)));
}

std::optional<StatePacket> UdpLoopbackReceiver::pollLatest() {
    std::optional<StatePacket> latest;
    std::array<std::uint8_t, 64> buffer{};
    for (;;) {
        const auto received = ::recv(impl_->socket.get(), reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            break;  // would block, or error: either way nothing more to read now
        }
        const auto size = static_cast<std::size_t>(received);
        if (auto packet = decodeStatePacket(std::span<const std::uint8_t>(buffer.data(), size))) {
            impl_->track(*packet);
            latest = packet;
        } else {
            WE_LOG_WARN_LIMITED(log(), impl_->malformedLimiter,
                                "receiver: dropped malformed datagram of {} bytes (expected {}, version {})", size,
                                StatePacket::kPacketSize, StatePacket::kVersion);
        }
    }
    return latest;
}

}  // namespace wheelemu
