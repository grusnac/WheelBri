#include "wheelbri/HelperProtocol.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/HideIds.hpp"

#include <algorithm>
#include <cctype>

namespace wheelbri::helper {

namespace {

void put16(std::vector<std::uint8_t>& b, const std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>(v >> 8U));
}

void put32(std::vector<std::uint8_t>& b, const std::uint32_t v) {
    put16(b, static_cast<std::uint16_t>(v & 0xFFFFU));
    put16(b, static_cast<std::uint16_t>(v >> 16U));
}

std::uint16_t get16(std::span<const std::uint8_t> b, const std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8U));
}

std::uint32_t get32(std::span<const std::uint8_t> b, const std::size_t at) {
    return static_cast<std::uint32_t>(get16(b, at)) | (static_cast<std::uint32_t>(get16(b, at + 2)) << 16U);
}

void putHeader(std::vector<std::uint8_t>& b, const std::uint32_t magic, const std::uint8_t kind, const std::uint32_t seq) {
    put32(b, magic);
    put16(b, kVersion);
    b.push_back(kind);
    b.push_back(0);
    put32(b, seq);
}

// Checks the header and returns the kind byte, or nullopt with `error` set.
std::optional<std::uint8_t> readHeader(std::span<const std::uint8_t> bytes, const std::uint32_t magic, std::string& error) {
    if (bytes.size() > kMaxMessage) {
        error = "message too long (" + std::to_string(bytes.size()) + " bytes)";
        return std::nullopt;
    }
    if (bytes.size() < kHeaderSize) {
        error = "message too short (" + std::to_string(bytes.size()) + " bytes)";
        return std::nullopt;
    }
    if (get32(bytes, 0) != magic) {
        error = "not a WheelBri helper message";
        return std::nullopt;
    }
    if (const std::uint16_t version = get16(bytes, 4); version != kVersion) {
        error = "protocol version " + std::to_string(version) + ", expected " + std::to_string(kVersion);
        return std::nullopt;
    }
    if (bytes[7] != 0) {
        error = "reserved byte set";
        return std::nullopt;
    }
    return bytes[6];
}

// A u16-length-prefixed string at `at`, moving `at` past it; nullopt when it runs past the end.
std::optional<std::string> readString(std::span<const std::uint8_t> bytes, std::size_t& at) {
    if (at + 2 > bytes.size()) return std::nullopt;
    const std::size_t length = get16(bytes, at);
    at += 2;
    if (at + length > bytes.size()) return std::nullopt;
    std::string text(reinterpret_cast<const char*>(bytes.data() + at), length);
    at += length;
    return text;
}

char upper(const char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

// Windows path, compared ignoring case: '/' -> '\', no "\\?\" prefix, no trailing separator.
std::string windowsPathKey(std::string_view path) {
    if (path.size() >= 4 && (path.substr(0, 4) == R"(\\?\)" || path.substr(0, 4) == "//?/")) path.remove_prefix(4);
    std::string key;
    key.reserve(path.size());
    for (const char c : path) key.push_back(c == '/' ? '\\' : upper(c));
    while (!key.empty() && key.back() == '\\') key.pop_back();
    return key;
}

bool hasDotPart(const std::string& key) {
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t end = std::min(key.find('\\', start), key.size());
        const std::string_view part(key.data() + start, end - start);
        if (part == "." || part == "..") return true;
        start = end + 1;
    }
    return false;
}

}  // namespace

std::vector<std::uint8_t> encode(const Request& request) {
    std::vector<std::uint8_t> b;
    putHeader(b, kRequestMagic, static_cast<std::uint8_t>(request.type), request.seq);
    const std::size_t count = std::min<std::size_t>(request.devices.size(), 255);
    b.push_back(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& id = request.devices[i];
        const std::size_t length = std::min<std::size_t>(id.size(), 0xFFFF);
        put16(b, static_cast<std::uint16_t>(length));
        b.insert(b.end(), id.begin(), id.begin() + static_cast<std::ptrdiff_t>(length));
    }
    return b;
}

std::vector<std::uint8_t> encode(const Reply& reply) {
    std::vector<std::uint8_t> b;
    putHeader(b, kReplyMagic, static_cast<std::uint8_t>(reply.status), reply.seq);
    const std::string text = truncateUtf8(reply.text, kMaxText);
    put16(b, static_cast<std::uint16_t>(text.size()));
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

std::optional<Request> decodeRequest(std::span<const std::uint8_t> bytes, std::string& error) {
    const auto kind = readHeader(bytes, kRequestMagic, error);
    if (!kind) return std::nullopt;
    Request request;
    request.type = static_cast<RequestType>(*kind);
    request.seq = get32(bytes, 8);
    std::size_t at = kHeaderSize;
    if (at >= bytes.size()) {
        error = "no device count";
        return std::nullopt;
    }
    const std::size_t count = bytes[at++];
    for (std::size_t i = 0; i < count; ++i) {
        auto id = readString(bytes, at);
        if (!id) {
            error = "device " + std::to_string(i + 1) + " of " + std::to_string(count) + " runs past the end";
            return std::nullopt;
        }
        request.devices.push_back(std::move(*id));
    }
    if (at != bytes.size()) {
        error = std::to_string(bytes.size() - at) + " bytes after the message";
        return std::nullopt;
    }
    return request;
}

std::optional<Reply> decodeReply(std::span<const std::uint8_t> bytes, std::string& error) {
    const auto kind = readHeader(bytes, kReplyMagic, error);
    if (!kind) return std::nullopt;
    if (*kind > static_cast<std::uint8_t>(Status::Unsupported)) {
        error = "unknown status " + std::to_string(*kind);
        return std::nullopt;
    }
    Reply reply;
    reply.status = static_cast<Status>(*kind);
    reply.seq = get32(bytes, 8);
    std::size_t at = kHeaderSize;
    auto text = readString(bytes, at);
    if (!text) {
        error = "text runs past the end";
        return std::nullopt;
    }
    if (at != bytes.size()) {
        error = std::to_string(bytes.size() - at) + " bytes after the message";
        return std::nullopt;
    }
    reply.text = std::move(*text);
    return reply;
}

bool isKnown(const RequestType type) {
    const auto value = static_cast<std::uint8_t>(type);
    return value >= static_cast<std::uint8_t>(RequestType::Hello) &&
           value <= static_cast<std::uint8_t>(RequestType::SetLogLevel);
}

std::optional<LogLevel> requestLogLevel(const Request& request) {
    if (request.devices.size() != 1) return std::nullopt;
    const std::optional<LogLevel> level = parseLogLevel(request.devices.front());
    if (!level || clampToApp(*level) != *level || request.devices.front() == "warning") return std::nullopt;
    return level;
}

std::string hideDeviceProblem(const std::string_view id) {
    if (id.empty()) return "empty device id";
    if (id.size() > kMaxIdLength) return "device id longer than " + std::to_string(kMaxIdLength) + " characters";
    for (const char c : id) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '&' || c == '_' || c == '.' ||
                        c == '#' || c == '{' || c == '}' || c == '-' || c == '\\';
        if (!ok) return "device id with a character not allowed in an instance id";
    }
    const std::size_t first = id.find('\\');
    const std::size_t second = first == std::string_view::npos ? first : id.find('\\', first + 1);
    if (second == std::string_view::npos || id.find('\\', second + 1) != std::string_view::npos || first == 0 ||
        second == first + 1 || second + 1 == id.size()) {
        return "not a device instance id (three parts: HID\\<hardware>\\<instance>)";
    }
    const auto usb = hideids::hidDeviceIdOf(id);
    if (!usb) return "not a HID device with a vendor and product id";
    if (!hideids::mayHide(*usb)) return "the virtual wheel (G920, G25) is never hidden";
    return {};
}

std::string requestProblem(const Request& request) {
    if (!isKnown(request.type)) {
        return "unknown request " + std::to_string(static_cast<int>(request.type));
    }
    if (request.type == RequestType::Hello) {
        return request.devices.empty() || requestLogLevel(request) ? std::string()
                                                                   : "Hello takes nothing but the app's log level";
    }
    if (request.type == RequestType::SetLogLevel) {
        return requestLogLevel(request) ? std::string() : "SetLogLevel takes one level: trace, debug, info or warn";
    }
    if (request.type == RequestType::CreateVirtualWheel) {
        return requestVirtualWheel(request) ? std::string()
                                            : "CreateVirtualWheel takes one wheel: g25, g27, g920 or g923";
    }
    if (request.type != RequestType::PutHideFilter) {
        return request.devices.empty() ? std::string() : std::string(requestName(request.type)) + " takes no devices";
    }
    if (request.devices.empty()) return "PutHideFilter without devices";
    if (request.devices.size() > kMaxDevices) {
        return "more than " + std::to_string(kMaxDevices) + " devices in one PutHideFilter";
    }
    for (const std::string& id : request.devices) {
        if (std::string problem = hideDeviceProblem(id); !problem.empty()) return problem;
    }
    return {};
}

std::optional<std::string> requestVirtualWheel(const Request& request) {
    if (request.devices.empty()) return std::string("g920");
    if (request.devices.size() == 1 && virtualWheel(request.devices[0]) != nullptr) return request.devices[0];
    return std::nullopt;
}

bool isInstalledApp(const std::string_view image, const std::string_view installFolder) {
    const std::string folder = windowsPathKey(installFolder);
    const std::string program = windowsPathKey(image);
    if (folder.empty() || hasDotPart(folder) || hasDotPart(program)) return false;
    return program == folder + "\\WHEELBRI.EXE" || program == folder + "\\WHEELBRI-SERVICE.EXE";
}

bool isWheelBriProgram(const std::string_view image) {
    const std::string key = windowsPathKey(image);
    const std::size_t cut = key.find_last_of('\\');
    const std::string name = cut == std::string::npos ? key : key.substr(cut + 1);
    return name == "WHEELBRI.EXE" || name == "WHEELBRI-SERVICE.EXE";
}

const char* requestName(const RequestType type) {
    switch (type) {
        case RequestType::Hello: return "Hello";
        case RequestType::CreateVirtualWheel: return "CreateVirtualWheel";
        case RequestType::RemoveVirtualWheel: return "RemoveVirtualWheel";
        case RequestType::HideRig: return "HideRig";
        case RequestType::ShowRig: return "ShowRig";
        case RequestType::PutHideFilter: return "PutHideFilter";
        case RequestType::InstallWheelDriver: return "InstallWheelDriver";
        case RequestType::ShareLogFolder: return "ShareLogFolder";
        case RequestType::SetLogLevel: return "SetLogLevel";
    }
    return "unknown";
}

const char* statusName(const Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::Refused: return "refused";
        case Status::Failed: return "failed";
        case Status::Pending: return "pending";
        case Status::Unsupported: return "unsupported";
    }
    return "unknown";
}

}  // namespace wheelbri::helper
