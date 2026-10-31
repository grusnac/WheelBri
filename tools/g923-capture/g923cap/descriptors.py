"""USB and HID descriptors: just enough to tell which interface carries what."""

from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class Endpoint:
    address: int
    attributes: int
    max_packet: int
    interval: int


@dataclass
class Interface:
    number: int
    alternate: int
    cls: int
    subclass: int
    protocol: int
    endpoints: list[Endpoint] = field(default_factory=list)
    hid_report_length: int | None = None
    hid_raw: bytes = b""


@dataclass
class DeviceDescriptor:
    vendor: int
    product: int
    bcd_device: int
    bcd_usb: int
    max_packet0: int
    raw: bytes


def parse_device(raw: bytes) -> DeviceDescriptor | None:
    if len(raw) < 18 or raw[1] != 0x01:
        return None
    bcd_usb, = struct.unpack_from("<H", raw, 2)
    vendor, product, bcd_device = struct.unpack_from("<HHH", raw, 8)
    return DeviceDescriptor(vendor, product, bcd_device, bcd_usb, raw[7], bytes(raw[:18]))


def parse_configuration(raw: bytes) -> list[Interface]:
    interfaces: list[Interface] = []
    pos = 0
    current: Interface | None = None
    while pos + 2 <= len(raw):
        length, kind = raw[pos], raw[pos + 1]
        if length < 2:
            break
        d = raw[pos:pos + length]
        if kind == 0x04 and len(d) >= 9:
            current = Interface(d[2], d[3], d[5], d[6], d[7])
            interfaces.append(current)
        elif kind == 0x05 and len(d) >= 7 and current is not None:
            current.endpoints.append(Endpoint(d[2], d[3], struct.unpack_from("<H", d, 4)[0], d[6]))
        elif kind == 0x21 and len(d) >= 9 and current is not None:
            current.hid_raw = bytes(d)
            n = d[5]
            for k in range(n):
                at = 6 + k * 3
                if at + 3 <= len(d) and d[at] == 0x22:
                    current.hid_report_length = struct.unpack_from("<H", d, at + 1)[0]
        pos += length
    return interfaces


@dataclass
class Collection:
    usage_page: int
    usage: int
    # report id -> bits of input, output, feature
    reports: dict[int, list[int]] = field(default_factory=dict)

    def report_bytes(self, report_id: int, which: int) -> int:
        bits = self.reports.get(report_id, [0, 0, 0])[which]
        return (bits + 7) // 8 + (1 if report_id else 0)


def parse_report_descriptor(raw: bytes) -> list[Collection]:
    """Top-level collections with their usage and report sizes (bits, report ID not counted)."""
    collections: list[Collection] = []
    usage_page = 0
    report_id = 0
    report_size = 0
    report_count = 0
    usages: list[int] = []
    depth = 0
    top: Collection | None = None
    stack: list[tuple[int, int, int, int]] = []
    pos = 0
    while pos < len(raw):
        prefix = raw[pos]
        if prefix == 0xFE:   # long item
            if pos + 1 >= len(raw):
                break
            pos += 3 + raw[pos + 1]
            continue
        size = (0, 1, 2, 4)[prefix & 3]
        kind = (prefix >> 2) & 3
        tag = prefix >> 4
        value_bytes = raw[pos + 1:pos + 1 + size]
        value = int.from_bytes(value_bytes, "little") if value_bytes else 0
        pos += 1 + size
        if kind == 1:   # global
            if tag == 0x0:
                usage_page = value
            elif tag == 0x7:
                report_size = value
            elif tag == 0x8:
                report_id = value
            elif tag == 0x9:
                report_count = value
            elif tag == 0xA:
                stack.append((usage_page, report_id, report_size, report_count))
            elif tag == 0xB and stack:
                usage_page, report_id, report_size, report_count = stack.pop()
        elif kind == 2:   # local
            if tag == 0x0:
                usages.append(value if size == 4 else (usage_page << 16) | value)
        elif kind == 0:   # main
            if tag == 0xA:   # collection
                if depth == 0:
                    first = usages[0] if usages else (usage_page << 16)
                    top = Collection(first >> 16, first & 0xFFFF)
                    collections.append(top)
                depth += 1
            elif tag == 0xC:
                depth = max(0, depth - 1)
            elif tag in (0x8, 0x9, 0xB) and top is not None:
                which = {0x8: 0, 0x9: 1, 0xB: 2}[tag]
                top.reports.setdefault(report_id, [0, 0, 0])[which] += report_size * report_count
            usages = []
    return collections


USAGE_PAGES = {
    0x01: "Generic Desktop",
    0x0F: "Physical Interface (PID)",
    0xFF00: "vendor FF00",
    0xFF43: "Logitech HID++ (FF43)",
    0xFFFD: "TrueForce (FFFD)",
}


def page_name(page: int) -> str:
    return USAGE_PAGES.get(page, f"0x{page:04X}")
