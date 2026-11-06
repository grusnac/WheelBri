// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// USBPcap captures (classic pcap or pcapng, link type 249) read into USB transfers, and written back (keep, the
// tests' fake wheel). Standard library only.
//
// USBPcap's packet header (USBPcap.h, little endian, packed, 27 bytes): headerLen u16, irpId u64, status u32,
// function u16, info u8 (bit 0: the completion, device -> host side), bus u16, device u16, endpoint u8 (bit 7 = IN),
// transfer u8 (0 iso, 1 interrupt, 2 control, 3 bulk), dataLength u32; control packets add a stage byte (0 setup,
// 1 data, 2 status, 3 complete). Since USBPcap 1.5 a control transfer is two packets: SETUP (8 setup bytes + OUT data)
// and COMPLETE (IN data). Interrupt OUT data rides on the request, interrupt IN data on the completion.
// A file cut short (the capture program stopped hard) is read up to its last whole packet.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wheelbri::g923cap {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint32_t kLinkTypeUsbPcap = 249;
inline constexpr std::uint8_t kTransferIso = 0;
inline constexpr std::uint8_t kTransferInterrupt = 1;
inline constexpr std::uint8_t kTransferControl = 2;
inline constexpr std::uint8_t kTransferBulk = 3;
inline constexpr std::uint8_t kStageSetup = 0;
inline constexpr std::uint8_t kStageData = 1;
inline constexpr std::uint8_t kStageStatus = 2;
inline constexpr std::uint8_t kStageComplete = 3;

// One frame of a capture file: where its bytes are in the file's buffer.
struct FrameRef {
    double ts = 0;   // seconds since 1970 (UTC)
    std::uint32_t linkType = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct CaptureFile {
    Bytes bytes;   // the whole file
    std::vector<FrameRef> frames;
    std::string error;   // not a pcap/pcapng file, or unreadable

    [[nodiscard]] std::span<const std::uint8_t> frame(const FrameRef& f) const {
        return std::span<const std::uint8_t>(bytes).subspan(f.offset, f.size);
    }
};

[[nodiscard]] CaptureFile readCapture(const std::filesystem::path& path);
// The same from bytes in memory (the fuzz target, the tests).
[[nodiscard]] CaptureFile parseCapture(Bytes bytes);

struct Urb {
    double ts = 0;
    std::uint64_t irp = 0;
    std::uint32_t status = 0;
    std::uint16_t function = 0;
    bool completion = false;   // info bit 0: an IRP coming back
    std::uint16_t bus = 0;
    std::uint16_t device = 0;
    std::uint8_t endpoint = 0;
    std::uint8_t transfer = 0;
    std::optional<std::uint8_t> stage;
    Bytes payload;
};

[[nodiscard]] std::optional<Urb> parseUrb(double ts, std::span<const std::uint8_t> frame);
// Every USBPcap frame of a file as a URB (other link types and broken frames left out); frame numbers are the
// index + 1, as Wireshark counts.
[[nodiscard]] std::vector<Urb> urbsOf(const CaptureFile& file);

enum class TransferKind { In, Out, Control };

// One USB transfer with its data.
struct Transfer {
    double ts = 0;
    TransferKind kind = TransferKind::In;
    std::uint16_t bus = 0;
    std::uint16_t device = 0;
    std::uint8_t endpoint = 0;
    Bytes data;      // IN data (interrupt IN, control IN), or OUT data (interrupt OUT)
    Bytes setup;     // control: the 8 setup bytes
    Bytes outData;   // control: data sent to the device
    std::int64_t status = 0;   // -1: a setup that never completed
    std::size_t frame = 0;     // the URB that completed it (Wireshark's numbering)

    [[nodiscard]] int requestType() const { return setup.empty() ? -1 : setup[0]; }
    [[nodiscard]] int request() const { return setup.size() > 1 ? setup[1] : -1; }
    [[nodiscard]] int value() const { return setup.size() >= 4 ? setup[2] | setup[3] << 8 : -1; }
    [[nodiscard]] int index() const { return setup.size() >= 6 ? setup[4] | setup[5] << 8 : -1; }
};

// URBs put together into transfers, in time order.
[[nodiscard]] std::vector<Transfer> transfersOf(const std::vector<Urb>& urbs);

// --- Writing -------------------------------------------------------------------------------------------------------

// A USBPcap frame (header, stage byte for control, payload).
[[nodiscard]] Bytes urbBytes(std::uint64_t irp, bool completion, std::uint16_t bus, std::uint16_t device,
                             std::uint8_t endpoint, std::uint8_t transfer, std::span<const std::uint8_t> payload,
                             std::optional<std::uint8_t> stage = std::nullopt, std::uint32_t status = 0);
// A classic pcap file of USBPcap frames (time, frame). False when it cannot be written.
bool writePcap(const std::filesystem::path& path, const std::vector<std::pair<double, Bytes>>& frames);

}  // namespace wheelbri::g923cap
