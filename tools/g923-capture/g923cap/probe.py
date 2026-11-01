"""S6: asks the real wheel itself, read-only. Windows only (ctypes on hid.dll and SetupAPI), no packages.

    python -m g923cap probe --out <folder> [--repo <WheelBri root>]

TrueForce interface (usage page 0xFFFD): 0x05 for parameters 0x00..0x3F, then 0x07 (slots). HID++ (usage page
0xFF43): Root getProtocolVersion and getFeature, FeatureSet getCount/getFeatureId, DeviceFwVersion getEntityCount/
getFwInfo, DeviceNameType getCount/getDeviceName/getDeviceType. Nothing else can be sent: SAFE_TRUFORCE_TYPES and
SAFE_HIDPP are checked before every write, and a test pins both lists.
"""

from __future__ import annotations

import json
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from . import PRODUCT_ID, VENDOR_ID
from . import hidpp
from . import truforce as tf

# Only 0x05 (a parameter's value). 0x07 (slots) is left out: that it only reads is known from a direct-drive wheel,
# not from a G923, and S2's capture holds the G923's answer to the game's own 0x07 anyway.
SAFE_TRUFORCE_TYPES = frozenset({0x05})
# (feature id, function) pairs that only read.
SAFE_HIDPP = frozenset({
    (0x0000, 0), (0x0000, 1),            # Root: getFeature, getProtocolVersion
    (0x0001, 0), (0x0001, 1),            # FeatureSet: getCount, getFeatureId
    (0x0003, 0), (0x0003, 1),            # DeviceFwVersion: getEntityCount, getFwInfo
    (0x0005, 0), (0x0005, 1), (0x0005, 2),   # DeviceNameType: getCount, getDeviceName, getDeviceType
})
SOFTWARE_ID = 0x0B          # ours, so the wheel's answers to G HUB are not taken for ours
DEVICE_INDEX = 0xFF         # a wheel on USB, not behind a receiver
SEQ_BASE = 0xA0


class Unsafe(Exception):
    pass


@dataclass
class HidInfo:
    path: str
    usage_page: int
    usage: int
    input_len: int
    output_len: int
    feature_len: int


class Transport:
    """What the probe needs from a HID collection (WindowsHid, or a fake in the tests)."""

    output_len: int = 64

    def write(self, data: bytes) -> None: ...

    def read(self, timeout_s: float) -> bytes | None: ...

    def close(self) -> None: ...


# The 4 bytes before the type: [report ID, 00, 00, 00] as WheelBri's notes have them. probe.run replaces them with the
# header the game's own 0x05 requests had in S2's capture, if there is one (the real wheel's report ID may differ).
HEADER = b"\x01\x00\x00\x00"


def truforce_request(rtype: int, seq: int, payload: bytes, length: int, seal: bool, header: bytes = HEADER) -> bytes:
    if rtype not in SAFE_TRUFORCE_TYPES:
        raise Unsafe(f"TrueForce type 0x{rtype:02X} is not a read")
    d = bytearray(length)
    d[0:4] = header[:4]
    d[4] = rtype
    d[5] = seq & 0xFF
    d[6:6 + len(payload)] = payload
    if seal:
        at = 6 + len(payload)
        d[at] = (0x100 - sum(d[4:at]) % 256) % 256
    return bytes(d)


def read_answer(t: Transport, match, timeout_s: float, seen: list | None = None) -> bytes | None:
    end = time.monotonic() + timeout_s
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return None
        d = t.read(left)
        if d is None:
            return None
        if match(d):
            return d
        if seen is not None and len(seen) < 2000:
            seen.append(d)


def probe_truforce(t: Transport, timeout_s: float = 0.3, indexes=range(0x40), header: bytes = HEADER) -> dict:
    res = {"parameters": {}, "no_answer": [], "slots": None, "sealed_requests": None, "other_reports": {}}
    seen: list[bytes] = []
    seq = SEQ_BASE

    def ask(rtype: int, payload: bytes) -> bytes | None:
        nonlocal seq
        tries = [res["sealed_requests"]] if res["sealed_requests"] is not None else [False, True]
        for seal in tries:
            seq = (seq + 1) & 0xFF
            s = seq
            t.write(truforce_request(rtype, s, payload, t.output_len, seal, header))
            a = read_answer(t, lambda d: len(d) > 5 and d[4] == rtype and d[5] == s, timeout_s, seen)
            if a is not None:
                res["sealed_requests"] = seal
                return a
        return None

    for idx in indexes:
        a = ask(0x05, bytes([idx]))
        if a is None:
            res["no_answer"].append(idx)
            continue
        parsed = tf.parameter_answer(a)
        if parsed:
            i, value, ok = parsed
            res["parameters"][f"0x{i:02X}"] = {"value": value, "well_formed": ok, "raw": a[6:14].hex(" ")}
    for d in seen:
        if len(d) > 5:
            key = f"0x{d[4]:02X}"
            res["other_reports"][key] = res["other_reports"].get(key, 0) + 1
    return res


def hidpp_request(feature_index: int, feature_id: int, function: int, params: bytes, length: int) -> bytes:
    if (feature_id, function) not in SAFE_HIDPP:
        raise Unsafe(f"HID++ feature 0x{feature_id:04X} function {function} is not on the read-only list")
    report_id = 0x11 if length == 20 else 0x12 if length == 64 else 0x10
    d = bytearray(length)
    d[0] = report_id
    d[1] = DEVICE_INDEX
    d[2] = feature_index
    d[3] = (function << 4) | SOFTWARE_ID
    d[4:4 + len(params)] = params[:length - 4]
    return bytes(d)


def probe_hidpp(t: Transport, timeout_s: float = 0.5) -> dict:
    res: dict = {"protocol": None, "features": {}, "firmware": [], "name": "", "device_type": None, "errors": []}

    def ask(index: int, fid: int, fn: int, params: bytes = b"") -> bytes | None:
        q = hidpp_request(index, fid, fn, params, t.output_len)
        t.write(q)
        a = read_answer(t, lambda d: hidpp.answers(q, d), timeout_s)
        if a is not None and hidpp.is_error(a):
            res["errors"].append({"feature": f"0x{fid:04X}", "function": fn,
                                  "error": hidpp.ERRORS.get(a[5], f"0x{a[5]:02X}")})
            return None
        return a

    a = ask(0, 0x0000, 1, b"\x00\x00\x5A")
    if a is not None:
        res["protocol"] = f"{a[4]}.{a[5]}"

    def index_of(fid: int) -> int | None:
        r = ask(0, 0x0000, 0, struct.pack(">H", fid))
        if r is None or r[4] == 0:
            return None
        res["features"][f"0x{fid:04X}"] = {"index": r[4], "type": r[5], "version": r[6]}
        return r[4]

    fs = index_of(0x0001)
    if fs is not None:
        c = ask(fs, 0x0001, 0)
        if c is not None:
            for i in range(1, c[4] + 1):
                r = ask(fs, 0x0001, 1, bytes([i]))
                if r is not None:
                    fid = r[4] << 8 | r[5]
                    res["features"].setdefault(f"0x{fid:04X}", {"index": i, "type": r[6],
                                                               "version": r[7] if len(r) > 7 else None})
    fw = index_of(0x0003)
    if fw is not None:
        c = ask(fw, 0x0003, 0)
        if c is not None:
            for e in range(c[4]):
                r = ask(fw, 0x0003, 1, bytes([e]))
                if r is not None:
                    res["firmware"].append({"entity": e, "type": r[4], "prefix": hidpp.ascii_of(r[5:8]),
                                            "number": f"{r[8]:02X}", "revision": f"{r[9]:02X}",
                                            "build": f"{r[10]:02X}{r[11]:02X}", "raw": r[4:20].hex(" ")})
    nm = index_of(0x0005)
    if nm is not None:
        c = ask(nm, 0x0005, 0)
        if c is not None:
            n, name = c[4], ""
            while len(name) < n:
                r = ask(nm, 0x0005, 1, bytes([len(name)]))
                if r is None:
                    break
                chunk = r[4:].split(b"\x00")[0].decode("ascii", "replace")
                if not chunk:
                    break
                name += chunk
            res["name"] = name[:n]
        r = ask(nm, 0x0005, 2)
        if r is not None:
            res["device_type"] = r[4]
    return res


def markdown(result: dict, wheelbri: dict[int, float]) -> str:
    lines = ["# Probe", "", f"{result.get('when', '')}; G HUB running: {result.get('ghub_running')}", ""]
    for k, v in result.get("collections", {}).items():
        lines.append(f"- {k}: {v}")
    t = result.get("truforce")
    if t:
        lines += ["", "## TrueForce parameters (0x05)", "",
                  f"Requests {'with' if t['sealed_requests'] else 'without'} a checksum got answers. "
                  f"No answer for: {' '.join(f'{i:02X}' for i in t['no_answer']) or 'none'}. "
                  f"Other reports while asking: {t['other_reports'] or 'none'}.", "",
                  "| Index | Real G923 | WheelBri | Raw |", "|---|---|---|---|"]
        for k in sorted(set(t["parameters"]) | {f"0x{i:02X}" for i in wheelbri}, key=lambda s: int(s, 16)):
            p = t["parameters"].get(k)
            ours = wheelbri.get(int(k, 16), 0.0)
            real = f"{p['value']:.6g}" if p else "no answer"
            differs = p is not None and abs(p["value"] - ours) > 1e-4 * max(1, abs(p["value"]))
            lines.append(f"| {k} | {'**' if differs else ''}{real}{'**' if differs else ''} | {ours:.6g} | "
                         f"`{p['raw'] if p else ''}` |")
        lines += ["", f"Slots (0x07): {t['slots']}"]
    h = result.get("hidpp")
    if h:
        lines += ["", "## HID++", "", f"Protocol {h['protocol']}; name '{h['name']}'; device type {h['device_type']}", ""]
        for fid, f in sorted(h["features"].items(), key=lambda kv: kv[1]["index"]):
            lines.append(f"- index {f['index']}: {hidpp.feature_name(int(fid, 16))} (type {f['type']}, "
                         f"version {f['version']})")
        for fw in h["firmware"]:
            lines.append(f"- firmware entity {fw['entity']}: type {fw['type']} {fw['prefix']} {fw['number']}."
                         f"{fw['revision']} build {fw['build']}")
        if h["errors"]:
            lines.append(f"- errors: {h['errors']}")
    for e in result.get("problems", []):
        lines.append(f"- problem: {e}")
    return "\n".join(lines) + "\n"


# --- Windows HID (ctypes) --------------------------------------------------------------------------------------------

def windows_collections(vendor: int = VENDOR_ID, product: int = PRODUCT_ID) -> list[HidInfo]:
    import ctypes
    from ctypes import wintypes

    hid = ctypes.WinDLL("hid")
    setupapi = ctypes.WinDLL("setupapi")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class GUID(ctypes.Structure):
        _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                    ("Data4", ctypes.c_ubyte * 8)]

    class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID), ("Flags", wintypes.DWORD),
                    ("Reserved", ctypes.c_void_p)]

    class HIDD_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT), ("ProductID", wintypes.USHORT),
                    ("VersionNumber", wintypes.USHORT)]

    class HIDP_CAPS(ctypes.Structure):
        _fields_ = [("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
                    ("InputReportByteLength", wintypes.USHORT), ("OutputReportByteLength", wintypes.USHORT),
                    ("FeatureReportByteLength", wintypes.USHORT), ("Reserved", wintypes.USHORT * 17),
                    ("rest", wintypes.USHORT * 10)]

    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
    setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), ctypes.c_wchar_p, ctypes.c_void_p,
                                              wintypes.DWORD]
    setupapi.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID),
                                                     wintypes.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
    setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                          ctypes.c_void_p, wintypes.DWORD,
                                                          ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
    hid.HidD_GetPreparsedData.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]

    DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
    infos = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    found: list[HidInfo] = []
    i = 0
    while True:
        data = SP_DEVICE_INTERFACE_DATA()
        data.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
        if not setupapi.SetupDiEnumDeviceInterfaces(infos, None, ctypes.byref(guid), i, ctypes.byref(data)):
            break
        i += 1
        needed = wintypes.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(infos, ctypes.byref(data), None, 0, ctypes.byref(needed), None)
        buf = ctypes.create_string_buffer(needed.value)
        ctypes.cast(buf, ctypes.POINTER(wintypes.DWORD))[0] = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        if not setupapi.SetupDiGetDeviceInterfaceDetailW(infos, ctypes.byref(data), buf, needed, None, None):
            continue
        path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
        low = path.lower()
        if f"vid_{vendor:04x}&pid_{product:04x}" not in low or "wheelbri" in low:
            continue
        h = kernel32.CreateFileW(path, 0, 3, None, 3, 0, None)   # no access: attributes and caps only
        if h in (None, wintypes.HANDLE(-1).value):
            continue
        try:
            pre = ctypes.c_void_p()
            caps = HIDP_CAPS()
            if hid.HidD_GetPreparsedData(h, ctypes.byref(pre)):
                hid.HidP_GetCaps(pre, ctypes.byref(caps))
                hid.HidD_FreePreparsedData(pre)
                found.append(HidInfo(path, caps.UsagePage, caps.Usage, caps.InputReportByteLength,
                                     caps.OutputReportByteLength, caps.FeatureReportByteLength))
        finally:
            kernel32.CloseHandle(h)
    setupapi.SetupDiDestroyDeviceInfoList(infos)
    return found


class WindowsHid(Transport):
    def __init__(self, info: HidInfo):
        import ctypes
        from ctypes import wintypes
        self._c, self._w = ctypes, wintypes
        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k.CreateFileW.restype = wintypes.HANDLE
        self.k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                       wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.k.CreateEventW.restype = wintypes.HANDLE
        self.k.ReadFile.restype = wintypes.BOOL
        self.k.WriteFile.restype = wintypes.BOOL
        self.k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p]
        self.k.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                                     ctypes.c_void_p]
        self.k.GetOverlappedResult.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD),
                                               wintypes.BOOL]
        self.k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self.k.CancelIo.argtypes = [wintypes.HANDLE]
        self.k.CloseHandle.argtypes = [wintypes.HANDLE]
        GENERIC_READ, GENERIC_WRITE, FILE_FLAG_OVERLAPPED = 0x80000000, 0x40000000, 0x40000000
        self.h = self.k.CreateFileW(info.path, GENERIC_READ | GENERIC_WRITE, 3, None, 3, FILE_FLAG_OVERLAPPED, None)
        if self.h in (None, wintypes.HANDLE(-1).value):
            raise OSError(f"cannot open {info.path} (error {ctypes.get_last_error()})")
        self.info = info
        self.output_len = info.output_len
        self.input_len = info.input_len

        class OVERLAPPED(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                        ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]
        self.OVERLAPPED = OVERLAPPED
        self.read_ov = None
        self.read_buf = None

    def _ov(self):
        ov = self.OVERLAPPED()
        ov.hEvent = self.k.CreateEventW(None, True, False, None)
        return ov

    def write(self, data: bytes) -> None:
        c, w = self._c, self._w
        buf = c.create_string_buffer(bytes(data), len(data))
        ov = self._ov()
        try:
            if not self.k.WriteFile(self.h, buf, len(data), None, c.byref(ov)):
                err = c.get_last_error()
                if err != 997:   # ERROR_IO_PENDING
                    raise OSError(f"write failed at once (error {err})")
            if self.k.WaitForSingleObject(ov.hEvent, 1000) != 0:
                self.k.CancelIo(self.h)
                raise OSError("write timed out")
            done = w.DWORD()
            if not self.k.GetOverlappedResult(self.h, c.byref(ov), c.byref(done), False):
                raise OSError(f"write failed (error {c.get_last_error()})")
        finally:
            self.k.CloseHandle(ov.hEvent)

    def read(self, timeout_s: float) -> bytes | None:
        c, w = self._c, self._w
        if self.read_ov is None:   # one read in flight at a time, kept across timeouts
            self.read_ov = self._ov()
            self.read_buf = c.create_string_buffer(self.input_len)
            if not self.k.ReadFile(self.h, self.read_buf, self.input_len, None, c.byref(self.read_ov)):
                err = c.get_last_error()
                if err != 997:   # ERROR_IO_PENDING
                    self.k.CloseHandle(self.read_ov.hEvent)
                    self.read_ov = None
                    raise OSError(f"read failed at once (error {err})")
        if self.k.WaitForSingleObject(self.read_ov.hEvent, max(1, int(timeout_s * 1000))) != 0:
            return None
        done = w.DWORD()
        ok = self.k.GetOverlappedResult(self.h, c.byref(self.read_ov), c.byref(done), False)
        data = self.read_buf.raw[:done.value] if ok else None
        self.k.CloseHandle(self.read_ov.hEvent)
        self.read_ov = None
        return data

    def close(self) -> None:
        if self.read_ov is not None:
            self.k.CancelIo(self.h)
            self.k.CloseHandle(self.read_ov.hEvent)
            self.read_ov = None
        self.k.CloseHandle(self.h)


def run(out: Path, repo: Path | None, ghub_running: str = "unknown") -> int:
    if sys.platform != "win32":
        print("the probe talks to the wheel through Windows' HID: run it on Windows")
        return 2
    out.mkdir(parents=True, exist_ok=True)
    result: dict = {"when": time.strftime("%Y-%m-%d %H:%M:%S"), "ghub_running": ghub_running, "collections": {},
                    "problems": []}
    cols = windows_collections()
    if not cols:
        print("no G923 (046D:C26E) HID collections found: is it plugged in, in PC mode?")
        return 1
    for c in cols:
        result["collections"][c.path] = (f"usage page 0x{c.usage_page:04X} usage 0x{c.usage:04X}, in/out/feature "
                                         f"{c.input_len}/{c.output_len}/{c.feature_len}")
        print(f"found: page 0x{c.usage_page:04X} usage 0x{c.usage:04X} in {c.input_len} out {c.output_len}")
    header = HEADER
    for analysis in sorted(out.parent.glob("S2*/analysis.json")):
        try:
            ex = json.loads(analysis.read_text(encoding="utf-8"))["truforce"]["examples"].get("0x05")
            if ex:
                header = bytes.fromhex(ex[0]["hex"].split(" …")[0].replace(" ", ""))[:4]
                print(f"TrueForce request header from {analysis.parent.name}: {header.hex(' ')}")
                break
        except (OSError, KeyError, ValueError, TypeError, IndexError):
            continue
    result["truforce_header"] = header.hex(" ")
    tfc = next((c for c in cols if c.usage_page == 0xFFFD), None)
    if tfc is None:
        result["problems"].append("no TrueForce collection (usage page 0xFFFD)")
    else:
        t = None
        try:
            t = WindowsHid(tfc)
            print("TrueForce: asking parameters 0x00..0x3F...")
            result["truforce"] = probe_truforce(t, header=header)
        except OSError as e:   # one part failing must not lose the other
            result["problems"].append(f"TrueForce probe: {e}")
            print(f"TrueForce probe failed: {e}")
        finally:
            if t is not None:
                t.close()
    hp = [c for c in cols if c.usage_page == 0xFF43 and c.output_len in (20, 64)]
    hp.sort(key=lambda c: c.output_len)   # long (20) first
    if not hp:
        result["problems"].append("no HID++ collection (usage page 0xFF43)")
    else:
        t = None
        try:
            t = WindowsHid(hp[0])
            print("HID++: protocol, features, firmware, name...")
            result["hidpp"] = probe_hidpp(t)
        except OSError as e:
            result["problems"].append(f"HID++ probe: {e}")
            print(f"HID++ probe failed: {e}")
        finally:
            if t is not None:
                t.close()
    wb = tf.wheelbri_parameters(repo) if repo else {}
    stem = "probe-ghub-" + ghub_running
    (out / f"{stem}.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    (out / f"{stem}.md").write_text(markdown(result, wb), encoding="utf-8")
    print(f"written: {out / (stem + '.md')}")
    return 0
