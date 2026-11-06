// SPDX-License-Identifier: GPL-3.0-or-later
#include "g923cap/Pcap.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>

namespace wheelbri::g923cap {

namespace {

constexpr std::size_t kUrbHeader = 27;

std::uint16_t u16(std::span<const std::uint8_t> d, const std::size_t at, const bool big = false) {
    return big ? static_cast<std::uint16_t>(d[at] << 8 | d[at + 1]) : static_cast<std::uint16_t>(d[at] | d[at + 1] << 8);
}

std::uint32_t u32(std::span<const std::uint8_t> d, const std::size_t at, const bool big = false) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(d[at + i]) << (big ? 8 * (3 - i) : 8 * i);
    }
    return v;
}

std::uint64_t u64(std::span<const std::uint8_t> d, const std::size_t at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(d[at + i]) << (8 * i);
    return v;
}

void parsePcap(CaptureFile& file) {
    const std::span<const std::uint8_t> d(file.bytes);
    if (d.size() < 24) {
        file.error = "too short for a pcap header";
        return;
    }
    const std::uint32_t le = u32(d, 0);
    const std::uint32_t be = u32(d, 0, true);
    bool big = false;
    std::uint32_t magic = le;
    if (le != 0xA1B2C3D4U && le != 0xA1B23C4DU) {
        if (be != 0xA1B2C3D4U && be != 0xA1B23C4DU) {
            file.error = "not a pcap or pcapng file";
            return;
        }
        big = true;
        magic = be;
    }
    const double divisor = magic == 0xA1B23C4DU ? 1e9 : 1e6;
    const std::uint32_t linkType = u32(d, 20, big);
    std::size_t pos = 24;
    while (pos + 16 <= d.size()) {
        const std::uint32_t sec = u32(d, pos, big);
        const std::uint32_t frac = u32(d, pos + 4, big);
        const std::uint32_t incl = u32(d, pos + 8, big);
        pos += 16;
        if (incl > d.size() - pos) break;   // cut short
        file.frames.push_back({static_cast<double>(sec) + static_cast<double>(frac) / divisor, linkType, pos, incl});
        pos += incl;
    }
}

void parsePcapng(CaptureFile& file) {
    const std::span<const std::uint8_t> d(file.bytes);
    std::size_t pos = 0;
    bool big = false;
    struct Interface {
        std::uint32_t linkType;
        double resolution;
    };
    std::vector<Interface> interfaces;
    while (pos + 12 <= d.size()) {
        if (d[pos] == 0x0A && d[pos + 1] == 0x0D && d[pos + 2] == 0x0D && d[pos + 3] == 0x0A) {
            big = !(d[pos + 8] == 0x4D && d[pos + 9] == 0x3C);
            interfaces.clear();
        }
        const std::uint32_t type = u32(d, pos, big);
        const std::uint32_t length = u32(d, pos + 4, big);
        if (length < 12 || length > d.size() - pos) break;
        const std::size_t body = pos + 8;
        const std::size_t bodySize = length - 12;
        if (type == 1 && bodySize >= 8) {   // interface description
            Interface itf{u16(d, body, big), 1e-6};
            std::size_t opt = body + 8;
            while (opt + 4 <= body + bodySize) {
                const std::uint16_t code = u16(d, opt, big);
                const std::uint16_t size = u16(d, opt + 2, big);
                if (code == 0) break;
                if (code == 9 && size >= 1 && opt + 4 < body + bodySize) {   // if_tsresol
                    const std::uint8_t r = d[opt + 4];
                    itf.resolution = (r & 0x80) != 0 ? std::pow(2.0, -(r & 0x7F)) : std::pow(10.0, -r);
                }
                opt += 4 + ((static_cast<std::size_t>(size) + 3) & ~std::size_t{3});
            }
            interfaces.push_back(itf);
        } else if (type == 6 && bodySize >= 20) {   // enhanced packet
            const std::uint32_t itf = u32(d, body, big);
            const std::uint64_t stamp = static_cast<std::uint64_t>(u32(d, body + 4, big)) << 32 | u32(d, body + 8, big);
            const std::uint32_t captured = u32(d, body + 12, big);
            if (itf < interfaces.size() && captured <= bodySize - 20) {
                file.frames.push_back({static_cast<double>(stamp) * interfaces[itf].resolution, interfaces[itf].linkType,
                                       body + 20, captured});
            }
        } else if (type == 3 && bodySize >= 4 && !interfaces.empty()) {   // simple packet
            const std::uint32_t original = u32(d, body, big);
            const std::size_t size = std::min<std::size_t>(original, bodySize - 4);
            file.frames.push_back({0.0, interfaces[0].linkType, body + 4, size});
        }
        pos += length;
    }
}

}  // namespace

CaptureFile parseCapture(Bytes bytes) {
    CaptureFile file;
    file.bytes = std::move(bytes);
    if (file.bytes.size() < 4) {
        file.error = "empty or too short";
        return file;
    }
    if (file.bytes[0] == 0x0A && file.bytes[1] == 0x0D && file.bytes[2] == 0x0D && file.bytes[3] == 0x0A) {
        parsePcapng(file);
    } else {
        parsePcap(file);
    }
    return file;
}

CaptureFile readCapture(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        CaptureFile file;
        file.error = "cannot open " + path.string();
        return file;
    }
    Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseCapture(std::move(bytes));
}

std::optional<Urb> parseUrb(const double ts, const std::span<const std::uint8_t> frame) {
    if (frame.size() < kUrbHeader) return std::nullopt;
    const std::uint16_t headerLen = u16(frame, 0);
    if (headerLen < kUrbHeader || headerLen > frame.size()) return std::nullopt;
    Urb u;
    u.ts = ts;
    u.irp = u64(frame, 2);
    u.status = u32(frame, 10);
    u.function = u16(frame, 14);
    u.completion = (frame[16] & 1) != 0;
    u.bus = u16(frame, 17);
    u.device = u16(frame, 19);
    u.endpoint = frame[21];
    u.transfer = frame[22];
    const std::uint32_t dataLength = u32(frame, 23);
    if (u.transfer == kTransferControl && headerLen > kUrbHeader) u.stage = frame[kUrbHeader];
    const std::size_t available = frame.size() - headerLen;
    const std::size_t n = std::min<std::size_t>(dataLength, available);
    u.payload.assign(frame.begin() + headerLen, frame.begin() + static_cast<std::ptrdiff_t>(headerLen + n));
    return u;
}

std::vector<Urb> urbsOf(const CaptureFile& file) {
    std::vector<Urb> urbs;
    urbs.reserve(file.frames.size());
    for (const FrameRef& f : file.frames) {
        if (f.linkType != kLinkTypeUsbPcap) continue;
        if (auto u = parseUrb(f.ts, file.frame(f))) urbs.push_back(std::move(*u));
    }
    return urbs;
}

std::vector<Transfer> transfersOf(const std::vector<Urb>& urbs) {
    std::vector<Transfer> out;
    out.reserve(urbs.size() / 2 + 1);
    std::map<std::pair<std::uint16_t, std::uint64_t>, std::pair<std::size_t, Urb>> pending;
    for (std::size_t i = 0; i < urbs.size(); ++i) {
        const Urb& u = urbs[i];
        const std::size_t frame = i + 1;
        if (u.transfer == kTransferControl) {
            const auto key = std::make_pair(u.bus, u.irp);
            if (u.stage == kStageSetup && !u.completion) {
                pending[key] = {frame, u};
            } else if (u.completion && (u.stage == kStageComplete || u.stage == kStageData || u.stage == kStageStatus)) {
                auto it = pending.find(key);
                if (it == pending.end() && u.stage != kStageComplete) continue;
                Transfer t;
                t.ts = u.ts;
                t.kind = TransferKind::Control;
                t.bus = u.bus;
                t.device = u.device;
                t.endpoint = u.endpoint;
                t.data = u.payload;
                t.status = u.status;
                t.frame = frame;
                if (it != pending.end()) {
                    const Bytes& p = it->second.second.payload;
                    t.setup.assign(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(8, p.size())));
                    if (p.size() > 8) t.outData.assign(p.begin() + 8, p.end());
                    pending.erase(it);
                }
                out.push_back(std::move(t));
            } else if (!u.completion && u.stage == kStageData) {
                // Before USBPcap 1.5: OUT data in its own packet after the setup.
                auto it = pending.find(key);
                if (it != pending.end()) {
                    Bytes& p = it->second.second.payload;
                    p.resize(std::min<std::size_t>(8, p.size()));
                    p.insert(p.end(), u.payload.begin(), u.payload.end());
                }
            }
        } else if (u.transfer == kTransferInterrupt || u.transfer == kTransferBulk) {
            const bool in = (u.endpoint & 0x80) != 0;
            if (in && u.completion && !u.payload.empty() && u.status == 0) {
                out.push_back({u.ts, TransferKind::In, u.bus, u.device, u.endpoint, u.payload, {}, {}, 0, frame});
            } else if (!in && !u.completion && !u.payload.empty()) {
                out.push_back({u.ts, TransferKind::Out, u.bus, u.device, u.endpoint, u.payload, {}, {}, 0, frame});
            }
        }
    }
    for (auto& [key, started] : pending) {   // a setup that never completed
        const Urb& s = started.second;
        Transfer t;
        t.ts = s.ts;
        t.kind = TransferKind::Control;
        t.bus = s.bus;
        t.device = s.device;
        t.endpoint = s.endpoint;
        t.setup.assign(s.payload.begin(), s.payload.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(8, s.payload.size())));
        if (s.payload.size() > 8) t.outData.assign(s.payload.begin() + 8, s.payload.end());
        t.status = -1;
        t.frame = started.first;
        out.push_back(std::move(t));
    }
    std::stable_sort(out.begin(), out.end(), [](const Transfer& a, const Transfer& b) {
        return a.ts != b.ts ? a.ts < b.ts : a.frame < b.frame;
    });
    return out;
}

Bytes urbBytes(const std::uint64_t irp, const bool completion, const std::uint16_t bus, const std::uint16_t device,
               const std::uint8_t endpoint, const std::uint8_t transfer, const std::span<const std::uint8_t> payload,
               const std::optional<std::uint8_t> stage, const std::uint32_t status) {
    const bool control = transfer == kTransferControl;
    const std::size_t headerLen = kUrbHeader + (control ? 1 : 0);
    Bytes b;
    b.reserve(headerLen + payload.size());
    const auto put = [&b](const std::uint64_t v, const int bytes) {
        for (int i = 0; i < bytes; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    put(headerLen, 2);
    put(irp, 8);
    put(status, 4);
    put(0, 2);   // URB function
    put(completion ? 1 : 0, 1);
    put(bus, 2);
    put(device, 2);
    put(endpoint, 1);
    put(transfer, 1);
    put(payload.size(), 4);
    if (control) b.push_back(stage.value_or(kStageSetup));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

bool writePcap(const std::filesystem::path& path, const std::vector<std::pair<double, Bytes>>& frames) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const auto put = [&out](const std::uint32_t v, const int bytes) {
        for (int i = 0; i < bytes; ++i) out.put(static_cast<char>(v >> (8 * i)));
    };
    put(0xA1B2C3D4U, 4);
    put(2, 2);
    put(4, 2);
    put(0, 4);
    put(0, 4);
    put(65535, 4);
    put(kLinkTypeUsbPcap, 4);
    for (const auto& [ts, frame] : frames) {
        const double whole = std::floor(ts);
        auto micro = static_cast<std::uint32_t>(std::llround((ts - whole) * 1e6));
        auto seconds = static_cast<std::uint32_t>(whole);
        if (micro >= 1000000U) {
            micro -= 1000000U;
            ++seconds;
        }
        put(seconds, 4);
        put(micro, 4);
        put(static_cast<std::uint32_t>(frame.size()), 4);
        put(static_cast<std::uint32_t>(frame.size()), 4);
        out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    }
    return static_cast<bool>(out);
}

}  // namespace wheelbri::g923cap
