"""A fake G923 for the tests: answers as WheelBri's TrueForceDevice does (core/src/TrueForce.cpp), and writes what a
USBPcap capture of it would hold."""

from __future__ import annotations

import struct

from g923cap.pcap import TRANSFER_CONTROL, TRANSFER_INTERRUPT, STAGE_COMPLETE, STAGE_SETUP, urb_bytes

PARAMS = {0x00: 2.0, 0x01: 1.0, 0x02: 32768.0, 0x03: 65535.0, 0x04: 65536.0, 0x05: 1.0, 0x06: 123.0, 0x07: 5.4054,
          0x08: 1.1, 0x09: 0.3, 0x0A: 1.0, 0x0B: 1.0, 0x0C: 47.1239, 0x0D: 1.5708, 0x0E: -9.4248, 0x0F: 9.4248,
          0x10: 13.0, 0x12: 4000.0, 0x14: 2000.0, 0x1D: 4.0, 0x30: 3.0, 0x33: 350.0, 0x36: -1.0}
COUNT_RANGE = 0x078000
FULL_SCALE = 0x88980000

DEVICE = bytes([18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x6D, 0x04, 0x6E, 0xC2, 0x00, 0x01, 1, 2, 3, 1])

# Report descriptors: game input (Generic Desktop, joystick), HID++ (FF43: 0x11 long, 0x12 very long),
# TrueForce (FFFD:FD01, report 1, 63 bytes each way).
RD_GAME = bytes([0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x08, 0x95, 0x0B, 0x81, 0x02, 0xC0])
RD_HIDPP = bytes([0x06, 0x43, 0xFF, 0x0A, 0x02, 0x06, 0xA1, 0x01, 0x85, 0x11, 0x75, 0x08, 0x95, 0x13, 0x15, 0x00,
                  0x26, 0xFF, 0x00, 0x09, 0x02, 0x81, 0x00, 0x09, 0x02, 0x91, 0x00, 0xC0,
                  0x06, 0x43, 0xFF, 0x0A, 0x04, 0x06, 0xA1, 0x01, 0x85, 0x12, 0x75, 0x08, 0x95, 0x3F, 0x09, 0x04,
                  0x81, 0x00, 0x09, 0x04, 0x91, 0x00, 0xC0])
RD_TF = bytes([0x06, 0xFD, 0xFF, 0x0A, 0x01, 0xFD, 0xA1, 0x01, 0x85, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x09, 0x01,
               0x81, 0x02, 0x09, 0x01, 0x91, 0x02, 0xC0])


def _iface(number: int, rd: bytes, ep_in: int, ep_out: int | None) -> bytes:
    n_ep = 2 if ep_out else 1
    d = bytes([9, 4, number, 0, n_ep, 3, 0, 0, 0])
    d += bytes([9, 0x21, 0x11, 0x01, 0, 1, 0x22]) + struct.pack("<H", len(rd))
    d += bytes([7, 5, ep_in, 3]) + struct.pack("<H", 64) + bytes([1])
    if ep_out:
        d += bytes([7, 5, ep_out, 3]) + struct.pack("<H", 64) + bytes([1])
    return d


def configuration() -> bytes:
    body = _iface(0, RD_GAME, 0x81, 0x01) + _iface(1, RD_HIDPP, 0x82, 0x02) + _iface(2, RD_TF, 0x83, 0x03)
    total = 9 + len(body)
    return bytes([9, 2]) + struct.pack("<H", total) + bytes([3, 1, 0, 0x80, 250]) + body


def seal(r: bytearray, at: int) -> None:
    r[at] = (0x100 - sum(r[4:at]) % 256) % 256


def blank(t: int, s: int) -> bytearray:
    r = bytearray(64)
    r[0] = 1
    r[4], r[5] = t, s
    return r


def status(t: int, s: int, position: int, clock: int, at17: int | None = None) -> bytes:
    r = blank(t, s)
    r[6:9] = position.to_bytes(3, "little")
    r[9:13] = b"\x00\x80\x00\x80"
    r[13:17] = (clock & 0xFFFFFFFF).to_bytes(4, "little")
    r[17:21] = ((at17 if at17 is not None else 0x80000000) & 0xFFFFFFFF).to_bytes(4, "little")
    r[22:25] = COUNT_RANGE.to_bytes(3, "little")
    r[25:29] = FULL_SCALE.to_bytes(4, "little")
    r[29:33] = FULL_SCALE.to_bytes(4, "little")
    seal(r, 33)
    return bytes(r)


def answer(req: bytes, position: int, clock: int, params=PARAMS) -> list[bytes]:
    t, s = req[4], req[5]
    if t == 0x05:
        idx = req[6]
        r = blank(5, s)
        r[6], r[7], r[8] = idx, 0x09, idx
        r[9:13] = struct.pack("<f", params.get(idx, 0.0))
        seal(r, 13)
        return [bytes(r)]
    if t == 0x07:
        r = blank(7, s)
        r[6] = 6
        for k, flag in enumerate([1, 1, 1, 1, 0, 1]):
            r[7 + 3 * k:10 + 3 * k] = bytes([k + 1, 1, flag])
        seal(r, 25)
        return [bytes(r)]
    if t == 0x06:
        return [status(0x10, s, position, clock, 0xFFFFFFFF)]
    if t == 0x0E:
        bits = int.from_bytes(req[6:10], "little")
        return [status(0x11, s, position, clock, bits), status(0x14, s, position, clock, bits)]
    if t in (0x03, 0x04, 0x01):
        return [status(0x02, s, position, clock)]
    return []


class Capture:
    """Builds the frames of a capture: descriptors, then whatever the test adds."""

    def __init__(self, t0: float = 1_800_000_000.0, bus: int = 1, address: int = 7):
        self.t = t0
        self.bus, self.addr = bus, address
        self.irp = 0x1000
        self.frames: list[tuple[float, bytes]] = []

    def _next(self) -> int:
        self.irp += 8
        return self.irp

    def control_in(self, setup: bytes, data: bytes, dt: float = 0.001) -> None:
        irp = self._next()
        self.frames.append((self.t, urb_bytes(irp, False, self.bus, self.addr, 0x80, TRANSFER_CONTROL, setup,
                                              STAGE_SETUP)))
        self.t += dt
        self.frames.append((self.t, urb_bytes(irp, True, self.bus, self.addr, 0x80, TRANSFER_CONTROL, data,
                                              STAGE_COMPLETE)))

    def descriptors(self, report_descriptors: bool = True) -> None:
        self.control_in(bytes([0x80, 6, 0, 1, 0, 0, 18, 0]), DEVICE)
        cfg = configuration()
        self.control_in(bytes([0x80, 6, 0, 2, 0, 0]) + struct.pack("<H", len(cfg)), cfg)
        if report_descriptors:
            for i, rd in enumerate((RD_GAME, RD_HIDPP, RD_TF)):
                self.control_in(bytes([0x81, 6, 0, 0x22, i, 0]) + struct.pack("<H", len(rd)), rd)

    def out(self, endpoint: int, data: bytes, dt: float = 0.0005) -> None:
        irp = self._next()
        self.frames.append((self.t, urb_bytes(irp, False, self.bus, self.addr, endpoint, TRANSFER_INTERRUPT, data)))
        self.t += dt
        self.frames.append((self.t, urb_bytes(irp, True, self.bus, self.addr, endpoint, TRANSFER_INTERRUPT, b"")))

    def inn(self, endpoint: int, data: bytes, dt: float = 0.0005) -> None:
        irp = self._next()
        self.frames.append((self.t, urb_bytes(irp, False, self.bus, self.addr, endpoint, TRANSFER_INTERRUPT, b"")))
        self.t += dt
        self.frames.append((self.t, urb_bytes(irp, True, self.bus, self.addr, endpoint, TRANSFER_INTERRUPT, data)))

    def wait(self, s: float) -> None:
        self.t += s


def tf_request(t: int, s: int, payload: bytes = b"") -> bytes:
    r = bytearray(64)
    r[0] = 1
    r[4], r[5] = t, s
    r[6:6 + len(payload)] = payload
    return bytes(r)


def hidpp_long(feature_index: int, function: int, sw: int, params: bytes) -> bytes:
    d = bytearray(20)
    d[0], d[1], d[2], d[3] = 0x11, 0xFF, feature_index, function << 4 | sw
    d[4:4 + len(params)] = params
    return bytes(d)


FEATURES = [0x0000, 0x0001, 0x0003, 0x0005, 0x8123]


def hidpp_answer(q: bytes) -> bytes:
    fi, fn = q[2], q[3] >> 4
    a = bytearray(q[:4]) + bytearray(16)
    if fi == 0 and fn == 0:
        fid = q[4] << 8 | q[5]
        a[4] = FEATURES.index(fid) if fid in FEATURES else 0
    elif fi == 0 and fn == 1:
        a[4], a[5], a[6] = 4, 2, q[6]
    elif fi == 1 and fn == 0:
        a[4] = len(FEATURES) - 1
    elif fi == 1 and fn == 1:
        fid = FEATURES[q[4]]
        a[4], a[5] = fid >> 8, fid & 0xFF
    elif fi == 2 and fn == 0:
        a[4] = 1
    elif fi == 2 and fn == 1:
        a[4:12] = bytes([0]) + b"U1 " + bytes([0x45, 0x01, 0x00, 0x23])
    elif fi == 3 and fn == 0:
        a[4] = 38
    elif fi == 3 and fn == 1:
        name = b"G923 Racing Wheel for Xbox One and PC"
        a[4:20] = name[q[4]:q[4] + 16].ljust(16, b"\x00")
    elif fi == 3 and fn == 2:
        a[4] = 0x0A
    elif fi == 4:
        pass   # ForceFeedback: acknowledged
    else:
        return bytes([0x11, 0xFF, 0xFF, fi, q[3], 6]) + bytes(14)
    return bytes(a)


def s2_like(path_pcap: str, path_notes: str, t0: float = 1_800_000_000.0, params=PARAMS) -> None:
    """A session like S2: the SDK's set-up, holds at known angles, a stream."""
    from g923cap.pcap import write_pcap
    c = Capture(t0)
    c.descriptors(report_descriptors=False)   # USBPcap injects device and configuration only
    position = COUNT_RANGE // 2
    clock = 0
    seq = 0

    def ask(t: int, payload: bytes = b"") -> None:
        nonlocal seq, clock
        seq = (seq + 1) & 0xFF
        req = tf_request(t, seq, payload)
        c.out(0x03, req)
        for a in answer(req, position, clock, params):
            c.inn(0x83, a)
        clock += 1000
    for idx in list(range(0x00, 0x30)):
        ask(0x05, bytes([idx]))
    ask(0x07)
    ask(0x06)
    ask(0x0E, struct.pack("<f", 900.0))
    ask(0x03)
    ask(0x0C, b"\x10\x00")
    ask(0x09)
    notes = []

    def hold(label: str, deg: float) -> None:
        nonlocal position, clock
        position = COUNT_RANGE // 2 + int(round(deg / 360.0 * 65536))
        notes.append((c.t, "HOLD_START", f"{label}|{deg:g}"))
        for _ in range(300):   # 3 s of status reports at 100/s
            c.inn(0x83, status(0x02, seq, position, clock))
            clock += 10000
            c.wait(0.0095)
        notes.append((c.t, "HOLD_END", label))
    hold("centre", 0)
    hold("left90", -90)
    hold("right90", 90)
    hold("left180", -180)
    for k in range(500):   # the stream: cur moves, texture window valid
        seq = (seq + 1) & 0xFF
        r = bytearray(tf_request(0x01, seq))
        cur = 0x8000 + int(8000 * ((k % 50) - 25) / 25)
        r[6:8] = cur.to_bytes(2, "little")
        r[10], r[11] = 2, 0x0D
        for slot in range(13):
            r[12 + slot * 4:14 + slot * 4] = (0x8000 + slot * 10).to_bytes(2, "little")
            r[14 + slot * 4:16 + slot * 4] = (0x8000 - slot * 10).to_bytes(2, "little")
        c.out(0x03, bytes(r), dt=0.0015)
    ask(0x04)
    write_pcap(path_pcap, c.frames)
    with open(path_notes, "w", encoding="utf-8") as f:
        for ts, kind, text in notes:
            f.write(f"{int(ts * 1000)}\t2027-01-15T10:00:00\t{kind}\t{text}\n")


def s1_like(path_pcap: str, t0: float = 1_800_000_000.0) -> None:
    from g923cap.pcap import write_pcap
    c = Capture(t0)
    c.descriptors(report_descriptors=True)
    for fid in (0x0001, 0x0003, 0x0005, 0x8123):
        q = hidpp_long(0, 0, 0x01, struct.pack(">H", fid))
        c.out(0x02, q)
        c.inn(0x82, hidpp_answer(q))
    q = hidpp_long(1, 0, 0x01, b"")
    c.out(0x02, q)
    c.inn(0x82, hidpp_answer(q))
    for i in range(1, len(FEATURES)):
        q = hidpp_long(1, 1, 0x01, bytes([i]))
        c.out(0x02, q)
        c.inn(0x82, hidpp_answer(q))
    q = hidpp_long(2, 1, 0x01, bytes([0]))
    c.out(0x02, q)
    c.inn(0x82, hidpp_answer(q))
    for off in (0, 16, 32):
        q = hidpp_long(3, 1, 0x01, bytes([off]))
        c.out(0x02, q)
        c.inn(0x82, hidpp_answer(q))
    for rot in (900, 540, 900):   # rotation set through 0x8123 (function 5 here, as a stand-in)
        q = hidpp_long(4, 5, 0x01, struct.pack(">H", rot))
        c.out(0x02, q)
        c.inn(0x82, hidpp_answer(q))
    write_pcap(path_pcap, c.frames)


class FakeTransport:
    """The probe's view of the fake wheel: TrueForce or HID++ answers, plus status noise."""

    def __init__(self, kind: str, needs_seal: bool = False, noise: int = 2):
        self.kind = kind
        self.output_len = 64 if kind == "tf" else 20
        self.queue: list[bytes] = []
        self.written: list[bytes] = []
        self.needs_seal = needs_seal
        self.noise = noise

    def write(self, data: bytes) -> None:
        self.written.append(bytes(data))
        if self.kind == "tf":
            for _ in range(self.noise):
                self.queue.append(status(0x02, 0x33, COUNT_RANGE // 2, 0))
            if self.needs_seal:
                end = 7 if data[4] == 0x05 else 6
                if sum(data[4:end + 1]) % 256 != 0:
                    return
            self.queue.extend(answer(data, COUNT_RANGE // 2, 0))
        else:
            self.queue.append(hidpp_answer(data))

    def read(self, timeout_s: float):
        return self.queue.pop(0) if self.queue else None

    def close(self) -> None:
        pass
