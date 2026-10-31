"""The TrueForce interface's packets and status reports, as WheelBri's core/include/wheelbri/TrueForce.hpp
describes them. Report ID 0x01, 64 bytes: [01 00 00 00, type, sequence, payload...]."""

from __future__ import annotations

import re
import struct
from pathlib import Path

# Where each known answer type carries its checksum (the bytes from [4] up to and including it sum to 0 mod 256).
SEAL_AT = {0x05: 13, 0x07: 25, 0x02: 33, 0x10: 33, 0x11: 33, 0x14: 33}
STATUS_TYPES = (0x02, 0x10, 0x11, 0x14)
# The answer types a request type gets from WheelBri (and, we expect, from a real wheel).
EXPECTED_ANSWERS = {0x05: (0x05,), 0x07: (0x07,), 0x06: (0x10,), 0x0E: (0x11, 0x14), 0x03: (0x02,), 0x04: (0x02,)}

TYPE_NAMES = {
    0x01: "stream (torque + texture)",
    0x03: "start session",
    0x04: "stop session",
    0x05: "parameter read",
    0x06: "0x06 (status 10 answer)",
    0x07: "slots",
    0x09: "0x09",
    0x0B: "0x0b",
    0x0C: "damping",
    0x0E: "operating range",
}

COUNTS_PER_TURN = 65536.0
TORQUE_UNIT = 65536000.0   # [25..28], [29..32]: int32 + 0x80000000, Nm * 65536000


def is_truforce_report(data: bytes) -> bool:
    return len(data) >= 6 and data[0] == 0x01 and data[1:4] == b"\x00\x00\x00"


def sealed_at(data: bytes, at: int) -> bool:
    return at < len(data) and sum(data[4:at + 1]) % 256 == 0


def find_seal(data: bytes) -> int | None:
    """The checksum position of an answer of unknown type: the last non-zero byte, if the bytes up to it sum to 0."""
    last = len(data) - 1
    while last > 5 and data[last] == 0:
        last -= 1
    if last > 5 and sealed_at(data, last):
        return last
    return None


def u24(d: bytes, at: int) -> int:
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16


def u32(d: bytes, at: int) -> int:
    return struct.unpack_from("<I", d, at)[0]


def f32(d: bytes, at: int) -> float:
    return struct.unpack_from("<f", d, at)[0]


def parameter_answer(d: bytes) -> tuple[int, float, bool] | None:
    """(index, value, well-formed) from a 0x05 answer: 05 s idx 09 idx <f32> <seal>."""
    if len(d) < 14 or d[4] != 0x05:
        return None
    ok = d[7] == 0x09 and d[8] == d[6] and sealed_at(d, 13)
    return d[6], f32(d, 9), ok


def status_fields(d: bytes) -> dict:
    t1, t2 = u32(d, 25), u32(d, 29)
    return {
        "type": d[4],
        "seq": d[5],
        "position": u24(d, 6),
        "at9": d[9:13].hex(" "),
        "clock_us": u32(d, 13),
        "at17": u32(d, 17),
        "speed": u32(d, 17) - 0x80000000,
        "at21": d[21],
        "count_range": u24(d, 22),
        "torque1": t1,
        "torque2": t2,
        "torque1_nm": (t1 - 0x80000000) / TORQUE_UNIT,
        "torque2_nm": (t2 - 0x80000000) / TORQUE_UNIT,
        "sealed": sealed_at(d, 33),
        "after_seal": d[34:].hex() if any(d[34:]) else "",
    }


def stream_fields(d: bytes) -> dict:
    cur = d[6] | d[7] << 8
    fields = {
        "seq": d[5],
        "cur_raw": cur,
        "cur": (cur - 0x8000) / 0x8000 if cur else 0.0,
        "b8_9": d[8:10].hex(),
        "new_samples": d[10],
        "window": d[11],
        "texture": [],
    }
    if d[11] == 0x0D and len(d) >= 12 + 13 * 4:
        for slot in range(13):
            at = 12 + slot * 4
            left = d[at] | d[at + 1] << 8
            right = d[at + 2] | d[at + 3] << 8
            fields["texture"].append((left, right))
    return fields


_CASE = re.compile(r"case\s+0x([0-9A-Fa-f]{2})\s*:\s*return\s+(-?[0-9.]+)F\s*;")


def wheelbri_parameters(repo_root: str | Path) -> dict[int, float]:
    """TrueForceDevice::parameter()'s values, read from core/src/TrueForce.cpp (empty when not found)."""
    path = Path(repo_root) / "core" / "src" / "TrueForce.cpp"
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    start = text.find("TrueForceDevice::parameter")
    if start < 0:
        return {}
    end = text.find("default:", start)
    return {int(i, 16): float(v) for i, v in _CASE.findall(text[start:end if end > 0 else None])}
