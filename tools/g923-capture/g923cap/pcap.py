"""Reads USBPcap captures (classic pcap or pcapng, link type 249) into USB transfers.

USBPcap's packet header (USBPcap.h, all little endian, packed): headerLen u16, irpId u64, status u32, function u16,
info u8 (bit 0: completion, device -> host side), bus u16, device u16, endpoint u8 (bit 7 = IN), transfer u8
(0 iso, 1 interrupt, 2 control, 3 bulk), dataLength u32; control packets add a stage byte (0 setup, 1 data,
2 status, 3 complete). Since USBPcap 1.5 a control transfer is two packets: SETUP (8 setup bytes + OUT data) and
COMPLETE (IN data). Interrupt OUT data rides on the request, interrupt IN data on the completion.
A file cut short (the capture program stopped hard) is read up to its last whole packet.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator

LINKTYPE_USBPCAP = 249

TRANSFER_ISO = 0
TRANSFER_INTERRUPT = 1
TRANSFER_CONTROL = 2
TRANSFER_BULK = 3

STAGE_SETUP = 0
STAGE_DATA = 1
STAGE_STATUS = 2
STAGE_COMPLETE = 3

_HEADER = struct.Struct("<HQIHBHHBBI")   # 27 bytes


class CaptureError(Exception):
    pass


def read_frames(path: str) -> Iterator[tuple[float, int, bytes]]:
    """(timestamp in seconds since 1970, link type, frame bytes) for every frame in a pcap or pcapng file."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 4:
        return
    magic = data[:4]
    if magic == b"\x0a\x0d\x0d\x0a":
        yield from _read_pcapng(data)
        return
    yield from _read_pcap(data)


def _read_pcap(data: bytes) -> Iterator[tuple[float, int, bytes]]:
    if len(data) < 24:
        raise CaptureError("too short for a pcap header")
    magic_le = struct.unpack("<I", data[:4])[0]
    if magic_le in (0xA1B2C3D4, 0xA1B23C4D):
        endian = "<"
    else:
        magic_be = struct.unpack(">I", data[:4])[0]
        if magic_be not in (0xA1B2C3D4, 0xA1B23C4D):
            raise CaptureError("not a pcap or pcapng file")
        endian = ">"
    magic = struct.unpack(endian + "I", data[:4])[0]
    divisor = 1e9 if magic == 0xA1B23C4D else 1e6
    linktype = struct.unpack(endian + "I", data[20:24])[0]
    pos = 24
    record = struct.Struct(endian + "IIII")
    while pos + 16 <= len(data):
        sec, frac, incl, _orig = record.unpack_from(data, pos)
        pos += 16
        if pos + incl > len(data):
            break   # cut short
        yield sec + frac / divisor, linktype, data[pos:pos + incl]
        pos += incl


def _read_pcapng(data: bytes) -> Iterator[tuple[float, int, bytes]]:
    pos = 0
    endian = "<"
    interfaces: list[tuple[int, float]] = []   # (link type, seconds per timestamp unit)
    while pos + 12 <= len(data):
        if data[pos:pos + 4] == b"\x0a\x0d\x0d\x0a":
            bom = data[pos + 8:pos + 12]
            endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
            interfaces = []
        block_type, block_len = struct.unpack_from(endian + "II", data, pos)
        if block_len < 12 or pos + block_len > len(data):
            break
        body = data[pos + 8:pos + block_len - 4]
        if block_type == 0x00000001 and len(body) >= 8:    # interface description
            linktype = struct.unpack_from(endian + "H", body, 0)[0]
            resolution = 1e-6
            opt = 8
            while opt + 4 <= len(body):
                code, length = struct.unpack_from(endian + "HH", body, opt)
                if code == 0:
                    break
                value = body[opt + 4:opt + 4 + length]
                if code == 9 and value:   # if_tsresol
                    r = value[0]
                    resolution = 2.0 ** -(r & 0x7F) if r & 0x80 else 10.0 ** -r
                opt += 4 + ((length + 3) & ~3)
            interfaces.append((linktype, resolution))
        elif block_type == 0x00000006 and len(body) >= 20:   # enhanced packet
            iface, ts_hi, ts_lo, captured, _orig = struct.unpack_from(endian + "IIIII", body, 0)
            if iface < len(interfaces):
                linktype, resolution = interfaces[iface]
                ts = ((ts_hi << 32) | ts_lo) * resolution
                yield ts, linktype, body[20:20 + captured]
        elif block_type == 0x00000003 and len(body) >= 4 and interfaces:   # simple packet
            linktype, _ = interfaces[0]
            orig = struct.unpack_from(endian + "I", body, 0)[0]
            yield 0.0, linktype, body[4:4 + orig]
        pos += block_len


@dataclass
class Urb:
    ts: float
    irp: int
    status: int
    function: int
    completion: bool        # info bit 0: the device -> host side (an IRP coming back)
    bus: int
    device: int
    endpoint: int
    transfer: int
    stage: int | None
    payload: bytes


def parse_urb(ts: float, frame: bytes) -> Urb | None:
    if len(frame) < _HEADER.size:
        return None
    header_len, irp, status, function, info, bus, device, endpoint, transfer, data_len = _HEADER.unpack_from(frame, 0)
    if header_len < _HEADER.size or header_len > len(frame):
        return None
    stage = frame[_HEADER.size] if transfer == TRANSFER_CONTROL and header_len > _HEADER.size else None
    payload = frame[header_len:header_len + data_len]
    return Urb(ts, irp, status, function, bool(info & 1), bus, device, endpoint, transfer, stage, payload)


def read_urbs(path: str) -> list[Urb]:
    urbs = []
    for ts, linktype, frame in read_frames(path):
        if linktype != LINKTYPE_USBPCAP:
            continue
        urb = parse_urb(ts, frame)
        if urb is not None:
            urbs.append(urb)
    return urbs


@dataclass
class Transfer:
    """One USB transfer with its data. kind: 'in' / 'out' (interrupt or bulk) or 'control'."""
    ts: float
    kind: str
    bus: int
    device: int
    endpoint: int
    data: bytes = b""
    setup: bytes = b""              # control: the 8 setup bytes
    out_data: bytes = b""           # control: data sent to the device
    status: int = 0
    frame: int = 0                  # index of the URB that completed it (for citing)
    extra: dict = field(default_factory=dict)

    @property
    def request_type(self) -> int:
        return self.setup[0] if self.setup else -1

    @property
    def request(self) -> int:
        return self.setup[1] if len(self.setup) > 1 else -1

    @property
    def value(self) -> int:
        return struct.unpack_from("<H", self.setup, 2)[0] if len(self.setup) >= 4 else -1

    @property
    def index(self) -> int:
        return struct.unpack_from("<H", self.setup, 4)[0] if len(self.setup) >= 6 else -1


def transfers(urbs: list[Urb]) -> list[Transfer]:
    """URBs put together into transfers, in time order. Frame numbers start at 1, as Wireshark counts."""
    out: list[Transfer] = []
    pending: dict[tuple[int, int], tuple[int, Urb]] = {}
    for i, u in enumerate(urbs, start=1):
        if u.transfer == TRANSFER_CONTROL:
            key = (u.bus, u.irp)
            if u.stage == STAGE_SETUP and not u.completion:
                pending[key] = (i, u)
            elif u.completion and u.stage in (STAGE_COMPLETE, STAGE_DATA, STAGE_STATUS):
                started = pending.pop(key, None)
                setup = started[1].payload[:8] if started else b""
                out_data = started[1].payload[8:] if started else b""
                if started is None and u.stage != STAGE_COMPLETE:
                    continue
                out.append(Transfer(u.ts, "control", u.bus, u.device, u.endpoint, data=u.payload, setup=setup,
                                    out_data=out_data, status=u.status, frame=i))
            elif not u.completion and u.stage == STAGE_DATA:
                # Before USBPcap 1.5: OUT data in its own packet after the setup.
                key_started = pending.get(key)
                if key_started is not None:
                    f0, s = key_started
                    pending[key] = (f0, Urb(s.ts, s.irp, s.status, s.function, s.completion, s.bus, s.device,
                                            s.endpoint, s.transfer, s.stage, s.payload[:8] + u.payload))
        elif u.transfer in (TRANSFER_INTERRUPT, TRANSFER_BULK):
            if u.endpoint & 0x80:
                if u.completion and u.payload and u.status == 0:
                    out.append(Transfer(u.ts, "in", u.bus, u.device, u.endpoint, data=u.payload, frame=i))
            elif not u.completion and u.payload:
                out.append(Transfer(u.ts, "out", u.bus, u.device, u.endpoint, data=u.payload, frame=i))
    for f0, s in pending.values():   # a setup that never completed
        out.append(Transfer(s.ts, "control", s.bus, s.device, s.endpoint, setup=s.payload[:8],
                            out_data=s.payload[8:], status=-1, frame=f0))
    out.sort(key=lambda t: (t.ts, t.frame))
    return out


# --- Writing (tests and the fake captures) -------------------------------------------------------------------------

def urb_bytes(irp: int, completion: bool, bus: int, device: int, endpoint: int, transfer: int, payload: bytes,
              stage: int | None = None, status: int = 0, function: int = 0) -> bytes:
    extra = bytes([stage]) if transfer == TRANSFER_CONTROL else b""
    header_len = _HEADER.size + len(extra)
    head = _HEADER.pack(header_len, irp, status, function, 1 if completion else 0, bus, device, endpoint, transfer,
                        len(payload))
    return head + extra + payload


def write_pcap(path: str, frames: list[tuple[float, bytes]]) -> None:
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_USBPCAP))
        for ts, frame in frames:
            sec = int(ts)
            usec = int(round((ts - sec) * 1e6))
            f.write(struct.pack("<IIII", sec, usec, len(frame), len(frame)))
            f.write(frame)
