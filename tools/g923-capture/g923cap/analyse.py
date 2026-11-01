"""Decodes one capture session (a folder with a .pcap/.pcapng, notes.txt and shim logs) or a whole day of them.

    python -m g923cap decode <session folder or day folder> [--repo <WheelBri root>]

Writes, per session: analysis.json, summary.md, stream.tsv (the 0x01 stream, if any); for a day folder also
report.md over all sessions. Everything it states cites capture frames (Wireshark's numbering).
"""

from __future__ import annotations

import bisect
import json
import re
import statistics
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

from . import OTHER_G923_IDS, PRODUCT_ID, VENDOR_ID, is_target, rehearsal_target
from . import descriptors as desc
from . import hidpp
from . import truforce as tf
from .pcap import Transfer, read_urbs, transfers

CAPTURE_SUFFIXES = (".pcap", ".pcapng")
ANSWER_WINDOW_S = 0.25


# --- Devices --------------------------------------------------------------------------------------------------------

@dataclass
class Device:
    bus: int
    address: int
    descriptor: desc.DeviceDescriptor | None = None
    descriptor_frame: int = 0
    interfaces: list[desc.Interface] = field(default_factory=list)
    configuration_frame: int = 0
    report_descriptors: dict[int, bytes] = field(default_factory=dict)   # interface -> raw
    report_descriptor_frames: dict[int, int] = field(default_factory=dict)
    transfers: int = 0
    truforce_like: int = 0

    @property
    def ident(self) -> str:
        if self.descriptor is None:
            return f"bus {self.bus} address {self.address} (no descriptor seen)"
        return (f"{self.descriptor.vendor:04X}:{self.descriptor.product:04X} on bus {self.bus} address "
                f"{self.address}")

    def collections(self, interface: int) -> list[desc.Collection]:
        raw = self.report_descriptors.get(interface)
        return desc.parse_report_descriptor(raw) if raw else []

    def interface_of_endpoint(self, endpoint: int) -> int | None:
        for i in self.interfaces:
            if any(e.address == endpoint for e in i.endpoints):
                return i.number
        return None

    def kind_of_interface(self, interface: int | None) -> str | None:
        """The interface's kind; "mixed" when its collections carry more than one kind (the real G923 has its game
        controller and vendor collections side by side on interface 0): its reports go by report ID then."""
        if interface is None:
            return None
        pages = {c.usage_page for c in self.collections(interface)}
        if not pages:
            return None
        kinds = {self._kind_of_page(p) for p in pages}
        return kinds.pop() if len(kinds) == 1 else "mixed"

    @staticmethod
    def _kind_of_page(page: int) -> str:
        if page == 0xFFFD:
            return "truforce"
        if page == 0xFF43:
            return "hidpp"
        return "vendor" if page >= 0xFF00 else "input"

    def kind_of_report(self, interface: int | None, report_id: int) -> str | None:
        """The kind of the collection that declares this report ID on this interface, when its descriptor is known."""
        if interface is None:
            return None
        for c in self.collections(interface):
            if report_id in c.reports:
                return self._kind_of_page(c.usage_page)
        return None


def find_devices(all_transfers: list[Transfer]) -> dict[tuple[int, int], Device]:
    devices: dict[tuple[int, int], Device] = {}
    for t in all_transfers:
        key = (t.bus, t.device)
        d = devices.setdefault(key, Device(t.bus, t.device))
        d.transfers += 1
        if t.kind in ("in", "out") and tf.is_truforce_report(t.data) and len(t.data) == 64:
            d.truforce_like += 1
        if t.kind != "control" or not t.setup:
            continue
        kind = t.value >> 8
        if t.request_type == 0x80 and t.request == 0x06 and t.data:
            if kind == 0x01:
                parsed = desc.parse_device(t.data)
                if parsed is not None:
                    d.descriptor, d.descriptor_frame = parsed, t.frame
            elif kind == 0x02 and len(t.data) >= 9:
                total = t.data[2] | t.data[3] << 8
                if len(t.data) >= total or not d.interfaces:
                    d.interfaces = desc.parse_configuration(t.data)
                    d.configuration_frame = t.frame
        elif t.request_type == 0x81 and t.request == 0x06 and kind == 0x22 and t.data:
            d.report_descriptors[t.index] = bytes(t.data)
            d.report_descriptor_frames[t.index] = t.frame
    return devices


def pick_target(devices: dict[tuple[int, int], Device], address: int | None = None) -> tuple[Device | None, str]:
    if address is not None:
        for d in devices.values():
            if d.address == address:
                return d, f"address {address}, as asked"
    rehearsal = rehearsal_target()
    if rehearsal is not None:
        for d in devices.values():
            if d.descriptor and is_target(d.descriptor.vendor, d.descriptor.product):
                return d, f"its device descriptor ({d.descriptor.vendor:04X}:{d.descriptor.product:04X}, rehearsal)"
        return None, "the rehearsal wheel is not in this capture"
    for d in devices.values():
        if d.descriptor and d.descriptor.vendor == VENDOR_ID and d.descriptor.product == PRODUCT_ID:
            return d, "its device descriptor (046D:C26E)"
    for d in devices.values():
        if d.descriptor and d.descriptor.vendor == VENDOR_ID and d.descriptor.product in OTHER_G923_IDS:
            return d, f"its device descriptor ({OTHER_G923_IDS[d.descriptor.product]})"
    candidates = [d for d in devices.values() if d.truforce_like]
    if candidates:
        best = max(candidates, key=lambda d: d.truforce_like)
        return best, "the most TrueForce-looking traffic (no descriptor in this capture)"
    return None, "no G923 in this capture"


# --- Reports --------------------------------------------------------------------------------------------------------

@dataclass
class Report:
    ts: float
    direction: str        # "out" (host -> wheel) or "in"
    interface: int | None
    data: bytes
    frame: int
    via: str              # "interrupt" or "SET_REPORT" / "GET_REPORT"


def reports_of(device: Device, all_transfers: list[Transfer]) -> list[Report]:
    out: list[Report] = []
    for t in all_transfers:
        if (t.bus, t.device) != (device.bus, device.address):
            continue
        if t.kind in ("in", "out"):
            out.append(Report(t.ts, t.kind, device.interface_of_endpoint(t.endpoint), bytes(t.data), t.frame,
                              "interrupt"))
        elif t.kind == "control" and t.request_type == 0x21 and t.request == 0x09 and t.out_data:
            out.append(Report(t.ts, "out", t.index, bytes(t.out_data), t.frame, "SET_REPORT"))
        elif t.kind == "control" and t.request_type == 0xA1 and t.request == 0x01 and t.data:
            out.append(Report(t.ts, "in", t.index, bytes(t.data), t.frame, "GET_REPORT"))
    return out


def interface_kinds(device: Device, reps: list[Report]) -> dict[int, str]:
    """Each interface's kind: from its report descriptor, or (none in this capture) from what most of its reports look
    like, so one game report that happens to look like HID++ does not land among the HID++ pairs."""
    kinds: dict[int, str] = {}
    counts: dict[int, Counter] = defaultdict(Counter)
    for r in reps:
        if r.interface is None:
            continue
        if r.direction == "out" and hidpp.is_hidpp(r.data):
            counts[r.interface]["hidpp_out"] += 1
        if len(r.data) == 64 and tf.is_truforce_report(r.data):
            counts[r.interface]["truforce"] += 1
        elif hidpp.is_hidpp(r.data):
            counts[r.interface]["hidpp"] += 1
        else:
            counts[r.interface]["input"] += 1
    for interface, c in counts.items():
        known = device.kind_of_interface(interface)
        if known is not None:
            kinds[interface] = known
        else:
            votes = Counter({k: v for k, v in c.items() if k != "hidpp_out"})
            kind, n = votes.most_common(1)[0]
            kinds[interface] = kind if n * 2 > sum(votes.values()) else "input"
            # HID++ sharing an interface with the game reports: the host writes HID++ there, a game report never
            # looks like that, so its HID++-shaped reports are HID++ all the same.
            if kinds[interface] == "input" and c["hidpp_out"]:
                kinds[interface] = "input+hidpp"
    return kinds


def classify(device: Device, r: Report, kinds: dict[int, str] | None = None) -> str:
    kind = device.kind_of_interface(r.interface)
    if kind == "mixed" and r.data:
        kind = device.kind_of_report(r.interface, r.data[0])
    if kind is None and kinds and r.interface in kinds:
        kind = kinds[r.interface]
        if kind == "input+hidpp":
            return "hidpp" if hidpp.is_hidpp(r.data) else "input"
    if kind == "truforce":
        return "truforce"
    if kind in ("hidpp", "vendor") and hidpp.is_hidpp(r.data):
        return "hidpp"
    if kind == "input":
        return "input"
    # No report descriptor for that interface: by the look of the report.
    if len(r.data) == 64 and tf.is_truforce_report(r.data):
        return "truforce"
    if hidpp.is_hidpp(r.data):
        return "hidpp"
    return "input"


# --- Notes and shim logs ----------------------------------------------------------------------------------------------

@dataclass
class Hold:
    label: str
    degrees: float
    start: float
    end: float


def read_notes(folder: Path) -> tuple[list[dict], list[Hold]]:
    path = folder / "notes.txt"
    lines: list[dict] = []
    holds: list[Hold] = []
    if not path.exists():
        return lines, holds
    open_holds: dict[str, tuple[float, float]] = {}
    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        parts = raw.split("\t")
        if len(parts) < 4 or not parts[0].strip().isdigit():
            continue
        ts = int(parts[0]) / 1000.0
        kind, text = parts[2].strip(), parts[3].strip()
        lines.append({"ts": ts, "local": parts[1], "kind": kind, "text": text})
        if kind == "HOLD_START" and "|" in text:
            label, deg = text.split("|", 1)
            try:
                open_holds[label] = (ts, float(deg))
            except ValueError:
                pass
        elif kind == "HOLD_END" and text in open_holds:
            start, deg = open_holds.pop(text)
            holds.append(Hold(text, deg, start, ts))
    return lines, holds


_SHIM_LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*?\b(logi[A-Za-z0-9_]+) #(\d+)")


def read_shim_logs(folder: Path) -> dict:
    calls: Counter[str] = Counter()
    first: dict[str, str] = {}
    files = sorted(folder.glob("wheelbri-shim-*.log"))
    for f in files:
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _SHIM_LINE.match(line)
            if m:
                calls[m.group(2)] += 1
                first.setdefault(m.group(2), m.group(1))
    return {"files": [f.name for f in files], "calls": dict(calls), "first": first}


# --- The analysis ---------------------------------------------------------------------------------------------------

def _hex(d: bytes, n: int = 40) -> str:
    h = d[:n].hex(" ")
    rest = d[n:]
    if rest:
        h += f" … (+{len(rest)} bytes{', all zero' if not any(rest) else ''})"
    return h


def analyse_truforce(reps: list[Report], holds: list[Hold], stream_path: Path | None) -> dict:
    outs = [r for r in reps if r.direction == "out"]
    ins = [r for r in reps if r.direction == "in"]
    res: dict = {"requests": {}, "answers": {}, "parameters": {}, "parameter_requests": [], "slots": [],
                 "status": {}, "holds": [], "stream": {}, "unanswered": [], "examples": {}}
    req_types = Counter(r.data[4] for r in outs if len(r.data) > 5)
    ans_types = Counter(r.data[4] for r in ins if len(r.data) > 5)
    res["requests"] = {f"0x{k:02X}": v for k, v in sorted(req_types.items())}
    res["answers"] = {f"0x{k:02X}": v for k, v in sorted(ans_types.items())}

    # Examples of every request type but the stream: the first few, raw.
    for r in outs:
        if len(r.data) > 5 and r.data[4] != 0x01:
            ex = res["examples"].setdefault(f"0x{r.data[4]:02X}", [])
            if len(ex) < 6:
                ex.append({"frame": r.frame, "via": r.via, "hex": _hex(r.data, 24)})

    # Answers by (type, seq), in time order, for pairing.
    by_type_seq: dict[tuple[int, int], list[Report]] = defaultdict(list)
    by_seq: dict[int, list[Report]] = defaultdict(list)
    for r in ins:
        if len(r.data) > 5:
            by_type_seq[(r.data[4], r.data[5])].append(r)
            if r.data[4] not in tf.STATUS_TYPES or r.data[4] != 0x02:
                by_seq[r.data[5]].append(r)

    def first_after(lst: list[Report], ts: float) -> Report | None:
        for a in lst:
            if ts - 0.002 <= a.ts <= ts + ANSWER_WINDOW_S:
                return a
        return None

    pairs: dict[str, dict] = {}
    for r in outs:
        if len(r.data) < 6 or r.data[4] == 0x01:
            continue
        rtype, seq = r.data[4], r.data[5]
        expected = tf.EXPECTED_ANSWERS.get(rtype)
        answer = None
        for at in (expected or (rtype,)):
            answer = first_after(by_type_seq.get((at, seq), []), r.ts)
            if answer:
                break
        if answer is None and expected is None:
            # Unknown type: anything with its sequence that is not the plain status.
            answer = first_after([a for a in by_seq.get(seq, []) if a.data[4] != 0x02], r.ts)
        p = pairs.setdefault(f"0x{rtype:02X}", {"sent": 0, "answered": 0, "answer_types": Counter(),
                                                 "first_answer": None, "first_request": None,
                                                 "delay_ms": []})
        p["sent"] += 1
        if p["first_request"] is None:
            p["first_request"] = {"frame": r.frame, "hex": _hex(r.data, 16)}
        if answer is not None:
            p["answered"] += 1
            p["answer_types"][f"0x{answer.data[4]:02X}"] += 1
            p["delay_ms"].append((answer.ts - r.ts) * 1000)
            if p["first_answer"] is None:
                seal = tf.SEAL_AT.get(answer.data[4]) or tf.find_seal(answer.data)
                p["first_answer"] = {"frame": answer.frame, "hex": _hex(answer.data, 34),
                                     "seal_at": seal,
                                     "sealed": bool(seal and tf.sealed_at(answer.data, seal))}
        elif len(res["unanswered"]) < 30:
            res["unanswered"].append({"frame": r.frame, "hex": _hex(r.data, 16)})
        if rtype == 0x05 and len(r.data) > 6:
            res["parameter_requests"].append(r.data[6])
    for p in pairs.values():
        p["answer_types"] = dict(p["answer_types"])
        d = p.pop("delay_ms")
        p["median_delay_ms"] = round(statistics.median(d), 2) if d else None
    res["pairs"] = pairs

    # Parameters, from every 0x05 answer.
    for r in ins:
        parsed = tf.parameter_answer(r.data)
        if parsed is None:
            continue
        idx, value, ok = parsed
        entry = res["parameters"].setdefault(f"0x{idx:02X}", {"value": value, "frame": r.frame, "well_formed": ok,
                                                             "raw": r.data[6:14].hex(" "), "values_seen": []})
        if value not in entry["values_seen"]:
            entry["values_seen"].append(value)

    for r in ins:
        if len(r.data) > 25 and r.data[4] == 0x07 and len(res["slots"]) < 3:
            n = r.data[6]
            slots = [list(r.data[7 + k * 3:10 + k * 3]) for k in range(min(n, 18))]
            res["slots"].append({"frame": r.frame, "count": n, "slots": slots, "hex": _hex(r.data, 30),
                                 "sealed": tf.sealed_at(r.data, 25)})

    # Status reports.
    statuses = [r for r in ins if len(r.data) >= 34 and r.data[4] in tf.STATUS_TYPES]
    for stype in tf.STATUS_TYPES:
        these = [r for r in statuses if r.data[4] == stype]
        if not these:
            continue
        fields = [tf.status_fields(r.data) for r in these]
        span = max(these[-1].ts - these[0].ts, 1e-9)
        clocks = [f["clock_us"] for f in fields]
        deltas = [b - a for a, b in zip(clocks, clocks[1:]) if b >= a]
        times = [r.ts for r in these]
        gaps = [(b - a) * 1e6 for a, b in zip(times, times[1:])]
        res["status"][f"0x{stype:02X}"] = {
            "count": len(these),
            "rate_per_s": round(len(these) / span, 1) if len(these) > 1 else None,
            "position_min": min(f["position"] for f in fields),
            "position_max": max(f["position"] for f in fields),
            "at9": dict(Counter(f["at9"] for f in fields).most_common(4)),
            "at17_examples": [f"0x{f['at17']:08X}" for f in fields[:3]],
            "at21": dict(Counter(f["at21"] for f in fields).most_common(3)),
            "count_range": dict(Counter(f["count_range"] for f in fields).most_common(3)),
            "torque1_nm": dict(Counter(round(f["torque1_nm"], 4) for f in fields).most_common(3)),
            "torque2_nm": dict(Counter(round(f["torque2_nm"], 4) for f in fields).most_common(3)),
            "torque_raw": f"0x{fields[0]['torque1']:08X} 0x{fields[0]['torque2']:08X}",
            "sealed": sum(1 for f in fields if f["sealed"]),
            "after_seal": dict(Counter(f["after_seal"] for f in fields).most_common(2)),
            "clock_median_step_us": statistics.median(deltas) if deltas else None,
            "host_median_gap_us": round(statistics.median(gaps), 1) if gaps else None,
            "speed_range": [min(f["speed"] for f in fields), max(f["speed"] for f in fields)],
            "first": {"frame": these[0].frame, "hex": _hex(these[0].data, 36)},
        }

    # Held angles: the median position in each hold (its first second left out: the hand settles).
    for h in holds:
        inside = [tf.u24(r.data, 6) for r in statuses if h.start + 1.0 <= r.ts <= h.end]
        res["holds"].append({"label": h.label, "degrees": h.degrees, "samples": len(inside),
                             "position": statistics.median(inside) if inside else None})
    centre = next((x["position"] for x in res["holds"] if x["degrees"] == 0 and x["position"] is not None), None)
    for x in res["holds"]:
        if centre is not None and x["position"] is not None and x["degrees"]:
            per_degree = (x["position"] - centre) / x["degrees"]
            x["counts_per_degree"] = round(per_degree, 3)
            x["counts_per_turn"] = round(per_degree * 360, 1)
    res["centre_position"] = centre

    # The stream.
    stream = [r for r in outs if len(r.data) >= 12 and r.data[4] == 0x01]
    if stream:
        fields = [tf.stream_fields(r.data) for r in stream]
        span = max(stream[-1].ts - stream[0].ts, 1e-9)
        curs = [f["cur"] for f in fields if f["cur_raw"]]
        peaks = [max(abs(l - 0x8000), abs(rr - 0x8000)) for f in fields for (l, rr) in f["texture"]]
        res["stream"] = {
            "count": len(stream),
            "rate_per_s": round(len(stream) / span, 1),
            "first_frame": stream[0].frame,
            "cur_range": [round(min(curs), 4), round(max(curs), 4)] if curs else None,
            "cur_none": sum(1 for f in fields if f["cur_raw"] == 0x8000),
            "bytes_10_11": dict(Counter(f"{f['new_samples']:02x}/{f['window']:02x}" for f in fields).most_common(8)),
            "bytes_8_9": dict(Counter(f["b8_9"] for f in fields).most_common(4)),
            "texture_peak_raw": max(peaks) if peaks else None,
            "via": dict(Counter(r.via for r in stream)),
        }
        if stream_path is not None:
            with open(stream_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("time_s\tframe\tseq\tcur_raw\tcur\tnew\twindow\ttexture_lr_hex\n")
                t0 = stream[0].ts
                for r, fl in zip(stream, fields):
                    tex = " ".join(f"{l:04x}/{rr:04x}" for l, rr in fl["texture"])
                    f.write(f"{r.ts - t0:.6f}\t{r.frame}\t{fl['seq']}\t{fl['cur_raw']}\t{fl['cur']:.5f}\t"
                            f"{fl['new_samples']}\t{fl['window']:02x}\t{tex}\n")
    return res


def analyse_hidpp(reps: list[Report]) -> dict:
    outs = [r for r in reps if r.direction == "out"]
    ins = [r for r in reps if r.direction == "in"]
    pairs = []
    used: set[int] = set()
    in_times = [a.ts for a in ins]
    for q in outs:
        answer = None
        for k in range(bisect.bisect_left(in_times, q.ts - 0.002), len(ins)):
            a = ins[k]
            if a.ts > q.ts + 1.0:
                break
            if id(a) not in used and hidpp.answers(q.data, a.data):
                answer = a
                used.add(id(a))
                break
        pairs.append((q, answer))
    feature_map: dict[int, int] = {0: 0x0000}
    for q, a in pairs:
        if a is None or hidpp.is_error(a.data):
            continue
        if q.data[2] == 0x00 and q.data[3] >> 4 == 0 and len(q.data) >= 6 and a.data[4] != 0:
            feature_map[a.data[4]] = q.data[4] << 8 | q.data[5]
    # FeatureSet getFeatureId answers fill in the rest.
    fs_index = next((i for i, f in feature_map.items() if f == 0x0001), None)
    if fs_index is not None:
        for q, a in pairs:
            if a is not None and not hidpp.is_error(a.data) and q.data[2] == fs_index and q.data[3] >> 4 == 1:
                feature_map[q.data[4]] = a.data[4] << 8 | a.data[5]
    rows: dict[tuple[int, int], dict] = {}
    for q, a in pairs:
        key = (q.data[2], q.data[3] >> 4)
        row = rows.setdefault(key, {"feature_index": q.data[2], "function": q.data[3] >> 4, "count": 0,
                                    "answered": 0, "errors": Counter(), "software_ids": Counter(),
                                    "examples": [], "report_ids": Counter()})
        row["count"] += 1
        row["software_ids"][q.data[3] & 0x0F] += 1
        row["report_ids"][f"0x{q.data[0]:02X}"] += 1
        if a is not None:
            row["answered"] += 1
            if hidpp.is_error(a.data):
                row["errors"][hidpp.ERRORS.get(a.data[5], f"0x{a.data[5]:02X}")] += 1
        params = q.data[4:].rstrip(b"\x00")
        sample = {"frame": q.frame, "params": params.hex(" "),
                  "answer": (a.data[4:].rstrip(b"\x00").hex(" ") if a else None),
                  "answer_frame": a.frame if a else None}
        if len(row["examples"]) < 4 and all(e["params"] != sample["params"] for e in row["examples"]):
            row["examples"].append(sample)
    out_rows = []
    for (fi, fn), row in sorted(rows.items()):
        row["errors"] = dict(row["errors"])
        row["software_ids"] = dict(row["software_ids"])
        row["report_ids"] = dict(row["report_ids"])
        out_rows.append(row)
    unsolicited = [a for a in ins if id(a) not in used]
    # Firmware and name, when they were asked for.
    firmware, name_chars = [], []
    for q, a in pairs:
        if a is None or hidpp.is_error(a.data):
            continue
        fid = feature_map.get(q.data[2])
        if fid == 0x0003 and q.data[3] >> 4 == 1:
            d = a.data
            firmware.append({"entity": q.data[4], "type": d[4], "prefix": hidpp.ascii_of(d[5:8]),
                             "number": f"{d[8]:02X}", "revision": f"{d[9]:02X}", "build": f"{d[10]:02X}{d[11]:02X}",
                             "raw": d[4:20].hex(" ")})
        if fid == 0x0005 and q.data[3] >> 4 == 1:
            name_chars.append((q.data[4], a.data[4:]))
    name = ""
    for _, chunk in sorted(name_chars):
        name += chunk.split(b"\x00")[0].decode("ascii", "replace")
    return {
        "requests": len(outs),
        "answered": sum(1 for _, a in pairs if a is not None),
        "unanswered": [{"frame": q.frame, "hex": _hex(q.data, 20)} for q, a in pairs if a is None][:20],
        "feature_map": {f"{k}": v for k, v in sorted(feature_map.items())},
        "rows": out_rows,
        "unsolicited": {"count": len(unsolicited), "examples": [{"frame": a.frame, "hex": _hex(a.data, 20)}
                                                               for a in unsolicited[:8]]},
        "firmware": firmware,
        "name": name,
        "via": dict(Counter(r.via for r in outs)),
    }


def analyse_input(reps: list[Report]) -> dict:
    ins = [r for r in reps if r.direction == "in"]
    outs = [r for r in reps if r.direction == "out"]
    return {
        "in": len(ins),
        "out": len(outs),
        "in_shapes": dict(Counter(f"id 0x{r.data[0]:02X}, {len(r.data)} bytes, interface {r.interface}"
                                  for r in ins).most_common(6)),
        "out_shapes": dict(Counter(f"id 0x{r.data[0]:02X}, {len(r.data)} bytes, interface {r.interface}, {r.via}"
                                   for r in outs if r.data).most_common(10)),
        "out_examples": [{"frame": r.frame, "hex": _hex(r.data, 24)} for r in outs[:10]],
    }


def describe_device(d: Device) -> dict:
    info: dict = {"ident": d.ident, "descriptor_frame": d.descriptor_frame,
                  "configuration_frame": d.configuration_frame}
    if d.descriptor:
        info["device_descriptor"] = d.descriptor.raw.hex(" ")
        info["bcd_device"] = f"{d.descriptor.bcd_device:04X}"
        info["bcd_usb"] = f"{d.descriptor.bcd_usb:04X}"
    info["interfaces"] = []
    for i in d.interfaces:
        cols = d.collections(i.number)
        info["interfaces"].append({
            "number": i.number, "alternate": i.alternate,
            "class": f"{i.cls:02X}/{i.subclass:02X}/{i.protocol:02X}",
            "endpoints": [f"0x{e.address:02X} attr {e.attributes:02X} max {e.max_packet} every {e.interval}"
                          for e in i.endpoints],
            "hid_descriptor": i.hid_raw.hex(" "),
            "report_descriptor_length": i.hid_report_length,
            "report_descriptor_seen": i.number in d.report_descriptors,
            "report_descriptor_frame": d.report_descriptor_frames.get(i.number),
            "report_descriptor": d.report_descriptors.get(i.number, b"").hex(" "),
            "collections": [{"usage_page": desc.page_name(c.usage_page), "usage": f"0x{c.usage:04X}",
                             "reports": {f"0x{rid:02X}": [c.report_bytes(rid, w) for w in range(3)]
                                         for rid in c.reports}} for c in cols],
            "kind": d.kind_of_interface(i.number),
        })
    return info


def analyse_session(folder: Path, repo: Path | None = None, address: int | None = None) -> dict:
    captures = sorted(p for p in folder.iterdir() if p.suffix.lower() in CAPTURE_SUFFIXES)
    result: dict = {"session": folder.name, "folder": str(folder), "captures": [p.name for p in captures]}
    notes, holds = read_notes(folder)
    result["notes"] = notes
    result["shim"] = read_shim_logs(folder)
    if not captures:
        result["error"] = "no .pcap or .pcapng in this folder"
        return result
    all_transfers: list[Transfer] = []
    chosen_capture = None
    target = None
    reason = ""
    # Several captures (one per root hub when the hub was not known): the one with the wheel.
    for cap in captures:
        tr = transfers(read_urbs(str(cap)))
        devs = find_devices(tr)
        dev, why = pick_target(devs, address)
        if dev is not None and (target is None or dev.descriptor is not None):
            all_transfers, chosen_capture, target, reason = tr, cap, dev, why
            devices = devs
            if dev.descriptor is not None:
                break
    if target is None:
        result["error"] = "no G923 found in " + ", ".join(p.name for p in captures)
        return result
    result["capture"] = chosen_capture.name
    result["target"] = describe_device(target)
    result["target"]["picked_by"] = reason
    result["other_devices"] = [d.ident for d in devices.values() if d is not target and d.descriptor]
    span = [t.ts for t in all_transfers]
    result["capture_span_s"] = round(span[-1] - span[0], 1) if span else 0
    result["capture_start"] = span[0] if span else None

    # Report descriptors come only with a plug-in (S1); the other sessions borrow them from the day's cache.
    cache = folder.parent / (f"descriptors-{target.descriptor.vendor:04X}-{target.descriptor.product:04X}.json"
                             if target.descriptor else "descriptors-unknown.json")
    if target.descriptor is not None:
        if target.report_descriptors:
            try:
                cache.write_text(json.dumps({str(k): v.hex() for k, v in target.report_descriptors.items()}),
                                 encoding="utf-8")
            except OSError:
                pass
        elif cache.exists():
            try:
                borrowed = json.loads(cache.read_text(encoding="utf-8"))
                target.report_descriptors = {int(k): bytes.fromhex(v) for k, v in borrowed.items()}
                result["report_descriptors_from"] = cache.name
            except (OSError, ValueError):
                pass
        result["target"] = describe_device(target)
        result["target"]["picked_by"] = reason

    reps = reports_of(target, all_transfers)
    kinds = interface_kinds(target, reps)
    result["interface_kinds"] = {str(k): v for k, v in kinds.items()}
    groups: dict[str, list[Report]] = defaultdict(list)
    for r in reps:
        groups[classify(target, r, kinds)].append(r)
    result["truforce"] = analyse_truforce(groups["truforce"], holds, folder / "stream.tsv")
    result["hidpp"] = analyse_hidpp(groups["hidpp"])
    result["input"] = analyse_input(groups["input"])
    if repo is not None:
        result["wheelbri_parameters"] = {f"0x{k:02X}": v for k, v in tf.wheelbri_parameters(repo).items()}
    return result


# --- Checks (each session's pass criteria) ---------------------------------------------------------------------------

def session_kind(name: str) -> str:
    m = re.match(r"(S\d)", name, re.IGNORECASE)
    return m.group(1).upper() if m else ""


def checks(result: dict) -> list[tuple[bool, str]]:
    out: list[tuple[bool, str]] = []
    if "error" in result:
        return [(False, result["error"])]
    kind = session_kind(result["session"])
    t = result["target"]
    tfr = result["truforce"]
    hp = result["hidpp"]
    pairs = tfr.get("pairs", {})

    def answered(rtype: str) -> tuple[bool, str]:
        p = pairs.get(rtype)
        if not p:
            return False, f"no {rtype} request"
        return p["answered"] == p["sent"], f"{rtype}: {p['answered']} of {p['sent']} answered"

    if rehearsal_target() is not None:
        return rehearsal_checks(result, kind)
    out.append((t.get("ident", "").startswith("046D:C26E"), f"the wheel: {t.get('ident')}"))
    if kind == "S1":
        out.append((bool(t.get("device_descriptor")), "device descriptor seen"))
        out.append((bool(t.get("interfaces")), f"configuration seen ({len(t.get('interfaces', []))} interfaces)"))
        hid = [i for i in t.get("interfaces", []) if i["class"].startswith("03")]
        missing = [str(i["number"]) for i in hid if not i["report_descriptor_seen"]]
        out.append((bool(hid) and not missing, "HID report descriptors seen for every HID interface"
                    + (f" (missing: interface {', '.join(missing)})" if missing else "")))
        out.append((hp["requests"] > 0 and hp["answered"] == hp["requests"],
                    f"HID++: {hp['answered']} of {hp['requests']} requests answered"))
        ffb_index = next((int(i) for i, f in hp["feature_map"].items() if f == 0x8123), None)
        ffb = [r for r in hp["rows"] if ffb_index is not None and r["feature_index"] == ffb_index]
        out.append((bool(ffb), "ForceFeedback (0x8123) requests seen (rotation, gain)" if ffb
                    else "no ForceFeedback (0x8123) request found"))
    elif kind in ("S2", "S4"):
        n_params = len(tfr["parameters"])
        ok, text = answered("0x05")
        out.append((ok and n_params > 0, f"parameters: {n_params} distinct; {text}"))
        out.append(answered("0x03"))
        # Measured with a real G923 (2026-11-01): the SDK sends no 0x06 or 0x07 to it, and the wheel does not answer
        # 0x0E (operating range). Reported, not judged.
        for rtype in ("0x06", "0x07", "0x0E", "0x0C"):
            p = pairs.get(rtype)
            out.append((True, f"{rtype}: " + (f"sent {p['sent']}, answered {p['answered']}" if p else "not sent")))
        st = tfr["status"]
        out.append((bool(st), "status reports: " + (", ".join(f"{k} x{v['count']}" for k, v in st.items()) or "none")))
        if kind == "S2":
            holds = [h for h in tfr["holds"] if h["position"] is not None]
            if len(holds) >= 3:
                by_deg = sorted(holds, key=lambda h: h["degrees"])
                positions = [h["position"] for h in by_deg]
                monotone = all(a < b for a, b in zip(positions, positions[1:])) or \
                    all(a > b for a, b in zip(positions, positions[1:]))
                out.append((monotone, "held angles: positions " + ", ".join(
                    f"{h['label']} {h['position']:.0f}" for h in by_deg)
                    + (" (in order)" if monotone else " (NOT in order: check the holds)")))
            else:
                out.append((False, f"held angles: only {len(holds)} with status reports (need centre, left, right)"))
        out.append((bool(tfr["stream"]), f"stream packets: {tfr['stream'].get('count', 0)}"))
        if kind == "S4":
            others = {k: v for k, v in pairs.items() if k not in ("0x05", "0x07", "0x06", "0x0E", "0x03", "0x04")}
            for k, v in others.items():
                out.append((True, f"{k}: sent {v['sent']}, answered {v['answered']} "
                                  f"(answer types {v['answer_types'] or 'none'})"))
    elif kind == "S3":
        setup = pairs.get("0x05", {}).get("sent", 0)
        out.append((True, f"with G HUB closed: {setup} parameter requests, "
                          f"{tfr['stream'].get('count', 0)} stream packets (information, not pass/fail)"))
    elif kind == "S5":
        ffb_index = next((int(i) for i, f in hp["feature_map"].items() if f == 0x8123), None)
        rows = [r for r in hp["rows"] if ffb_index is None or r["feature_index"] == ffb_index]
        total = sum(r["count"] for r in rows)
        out.append((total > 0, f"HID++ effects traffic: {total} requests"
                    + ("" if ffb_index is not None else " (feature map unknown here: all HID++ counted)")))
    shim = result["shim"]
    if kind in ("S2", "S3", "S4"):
        out.append((bool(shim["calls"]), "shim log: " + (", ".join(f"{k} x{v}" for k, v in sorted(
            shim["calls"].items())) or "no logi* calls (shim missing, or logi_log off?)")))
    return out


def rehearsal_checks(result: dict, kind: str) -> list[tuple[bool, str]]:
    """A rehearsal with another wheel (G923_TARGET): only what any wheel shows. TrueForce and HID++ are G923 things."""
    t = result["target"]
    inp = result["input"]
    out = [(True, f"REHEARSAL with {t.get('ident')} (TrueForce and HID++ checks left out)")]
    out.append((bool(t.get("device_descriptor")), "device descriptor seen"))
    out.append((bool(t.get("interfaces")), f"configuration seen ({len(t.get('interfaces', []))} interfaces)"))
    if kind == "S1":
        hid = [i for i in t.get("interfaces", []) if i["class"].startswith("03")]
        missing = [str(i["number"]) for i in hid if not i["report_descriptor_seen"]]
        out.append((bool(hid) and not missing, "HID report descriptors seen for every HID interface"
                    + (f" (missing: interface {', '.join(missing)})" if missing else "")))
    out.append((inp["in"] > 0, f"input reports from the wheel: {inp['in']}"))
    out.append((True, f"reports to the wheel (force feedback): {inp['out']}"))
    holds = [n for n in result.get("notes", []) if n["kind"] in ("HOLD_START", "HOLD_END")]
    if kind == "S2":
        out.append((len(holds) >= 8, f"holds noted: {len(holds) // 2}"))
    if kind in ("S2", "S3", "S4"):
        shim = result["shim"]
        out.append((True, f"shim logs copied: {len(shim['files'])}"))
    return out


# --- Writing -------------------------------------------------------------------------------------------------------

def _fmt(v) -> str:
    if isinstance(v, float):
        return f"{v:.6g}"
    return str(v)


def summary_markdown(result: dict, feature_names: dict[int, int] | None = None) -> str:
    lines = [f"# {result['session']}", ""]
    for ok, text in checks(result):
        lines.append(f"- {'PASS' if ok else '**FAIL**'}: {text}")
    if "error" in result:
        return "\n".join(lines) + "\n"
    t = result["target"]
    lines += ["", f"Capture `{result['capture']}`, {result['capture_span_s']} s. Wheel: {t['ident']} "
              f"(picked by {t['picked_by']}).", ""]
    if t.get("device_descriptor"):
        lines += [f"Device descriptor (frame {t['descriptor_frame']}): `{t['device_descriptor']}` "
                  f"(bcdDevice {t['bcd_device']}, USB {t['bcd_usb']})", ""]
    if t["interfaces"]:
        lines += ["## Interfaces", "", "| # | Class | Endpoints | Report descriptor | Collections | Kind |",
                  "|---|---|---|---|---|---|"]
        for i in t["interfaces"]:
            cols = "; ".join(f"{c['usage_page']} {c['usage']} reports "
                             + ", ".join(f"{rid} in/out/feat {v[0]}/{v[1]}/{v[2]} B" for rid, v in c["reports"].items())
                             for c in i["collections"]) or "-"
            rd = (f"{i['report_descriptor_length']} B, frame {i['report_descriptor_frame']}"
                  if i["report_descriptor_seen"] else f"{i['report_descriptor_length']} B, not in this capture")
            lines.append(f"| {i['number']} | {i['class']} | {'<br>'.join(i['endpoints'])} | {rd} | {cols} | "
                         f"{i['kind'] or '?'} |")
        lines.append("")
        for i in t["interfaces"]:
            if i["hid_descriptor"]:
                lines.append(f"- Interface {i['number']} HID descriptor: `{i['hid_descriptor']}`")
        lines.append("")

    tfr = result["truforce"]
    if tfr["requests"] or tfr["answers"]:
        lines += ["## TrueForce", "", f"Requests by type: {tfr['requests']}. Answers by type: {tfr['answers']}.", "",
                  "| Request | Sent | Answered | Answer types | Delay (median) | First request | First answer |",
                  "|---|---|---|---|---|---|---|"]
        for k, p in sorted(tfr["pairs"].items()):
            fa = p["first_answer"]
            fa_text = (f"frame {fa['frame']}: `{fa['hex']}` (seal at {fa['seal_at']}, "
                       f"{'ok' if fa['sealed'] else 'NOT sealed'})") if fa else "-"
            lines.append(f"| {k} {tf.TYPE_NAMES.get(int(k, 16), '')} | {p['sent']} | {p['answered']} | "
                         f"{p['answer_types'] or '-'} | {p['median_delay_ms']} ms | frame {p['first_request']['frame']}: "
                         f"`{p['first_request']['hex']}` | {fa_text} |")
        lines.append("")
        if tfr["parameters"]:
            wb = result.get("wheelbri_parameters", {})
            lines += ["### Parameters (0x05)", "",
                      f"Asked for, in order: {' '.join(f'{i:02X}' for i in tfr['parameter_requests'][:64])}"
                      + (" …" if len(tfr["parameter_requests"]) > 64 else ""), "",
                      "| Index | Real G923 | WheelBri | Frame | Raw (idx 09 idx f32 seal) |", "|---|---|---|---|---|"]
            for k in sorted(set(tfr["parameters"]) | set(wb), key=lambda s: int(s, 16)):
                p = tfr["parameters"].get(k)
                real = _fmt(p["value"]) if p else "not asked"
                if p and len(p["values_seen"]) > 1:
                    real += f" (also {', '.join(_fmt(v) for v in p['values_seen'][1:])})"
                ours = _fmt(wb.get(k, 0.0)) if wb else "?"
                differs = p is not None and wb and abs(p["value"] - wb.get(k, 0.0)) > 1e-4 * max(1, abs(p["value"]))
                mark = "**" if differs else ""
                lines.append(f"| {k} | {mark}{real}{mark} | {ours} | {p['frame'] if p else '-'} | "
                             f"`{p['raw'] if p else ''}`{'' if not p or p['well_formed'] else ' (odd form)'} |")
            lines.append("")
        for s in tfr["slots"][:1]:
            lines += [f"### Slots (0x07), frame {s['frame']}", "", f"Count {s['count']}: "
                      + ", ".join(f"[{' '.join(f'{b:02X}' for b in sl)}]" for sl in s["slots"])
                      + f"; sealed: {s['sealed']}. Raw `{s['hex']}`", ""]
        if tfr["status"]:
            lines += ["### Status reports", "",
                      "| Type | Count | Rate/s | Position min..max | [9..12] | [17..20] first | [21] | Count range "
                      "[22..24] | Full scale [25..28] / [29..32] Nm | Sealed | Clock step (us) | First |",
                      "|---|---|---|---|---|---|---|---|---|---|---|---|"]
            for k, s in tfr["status"].items():
                lines.append(f"| {k} | {s['count']} | {s['rate_per_s']} | {s['position_min']}..{s['position_max']} | "
                             f"{s['at9']} | {', '.join(s['at17_examples'])} | {s['at21']} | {s['count_range']} | "
                             f"{s['torque1_nm']} / {s['torque2_nm']} (`{s['torque_raw']}`) | {s['sealed']} | "
                             f"{s['clock_median_step_us']} | frame {s['first']['frame']}: `{s['first']['hex']}` |")
            lines.append("")
        if tfr["holds"]:
            lines += ["### Held angles", "", f"Centre position: {tfr['centre_position']}", "",
                      "| Hold | Degrees (+ right) | Samples | Median position | Counts/degree | Counts/turn |",
                      "|---|---|---|---|---|---|"]
            for h in tfr["holds"]:
                lines.append(f"| {h['label']} | {h['degrees']} | {h['samples']} | {h['position']} | "
                             f"{h.get('counts_per_degree', '-')} | {h.get('counts_per_turn', '-')} |")
            lines += ["", "WheelBri assumes 65536 counts a turn (182.04 a degree) and a right turn counting up.", ""]
        if tfr["stream"]:
            s = tfr["stream"]
            lines += ["### Stream (0x01)", "",
                      f"{s['count']} packets, {s['rate_per_s']}/s, first at frame {s['first_frame']} ({s['via']}). "
                      f"cur range {s['cur_range']}, 'none' (0x8000) x{s['cur_none']}. Bytes 10/11: "
                      f"{s['bytes_10_11']}. Bytes 8..9: {s['bytes_8_9']}. Texture peak (raw from 0x8000): "
                      f"{s['texture_peak_raw']}. Every packet: `stream.tsv`.", ""]
        if tfr["unanswered"]:
            lines += ["Unanswered requests (first ones): " + "; ".join(
                f"frame {u['frame']} `{u['hex']}`" for u in tfr["unanswered"][:10]), ""]
        if tfr["examples"]:
            lines += ["### Request examples", ""]
            for k, ex in sorted(tfr["examples"].items()):
                lines.append(f"- {k}: " + "; ".join(f"frame {e['frame']} ({e['via']}) `{e['hex']}`" for e in ex))
            lines.append("")

    hp = result["hidpp"]
    if hp["requests"] or hp["unsolicited"]["count"]:
        fmap = {int(k): v for k, v in hp["feature_map"].items()}
        if feature_names:
            for k, v in feature_names.items():
                fmap.setdefault(k, v)
        lines += ["## HID++", "", f"{hp['requests']} requests, {hp['answered']} answered "
                  f"({hp['via']}); {hp['unsolicited']['count']} reports from the wheel nobody asked for.", ""]
        if hp["name"]:
            lines.append(f"Name: {hp['name']}")
        for fw in hp["firmware"]:
            lines.append(f"Firmware entity {fw['entity']}: type {fw['type']} {fw['prefix']} {fw['number']}."
                         f"{fw['revision']} build {fw['build']} (`{fw['raw']}`)")
        lines += ["", "Feature map (index: id): " + ", ".join(f"{i}: {hidpp.feature_name(f)}"
                                                           for i, f in sorted(fmap.items())), "",
                  "| Index | Feature | Function | Count | Answered | Errors | sw ids | Reports | Examples (params -> answer) |",
                  "|---|---|---|---|---|---|---|---|---|"]
        for r in hp["rows"]:
            ex = "<br>".join(f"f{e['frame']}: `{e['params'] or '-'}` -> `{e['answer']}`" for e in r["examples"])
            lines.append(f"| {r['feature_index']} | {hidpp.feature_name(fmap.get(r['feature_index']))} | "
                         f"{r['function']} | {r['count']} | {r['answered']} | {r['errors'] or '-'} | "
                         f"{r['software_ids']} | {r['report_ids']} | {ex} |")
        if hp["unsolicited"]["examples"]:
            lines += ["", "Unsolicited: " + "; ".join(f"frame {e['frame']} `{e['hex']}`"
                                                      for e in hp["unsolicited"]["examples"])]
        lines.append("")
    inp = result["input"]
    lines += ["## Other reports", "", f"In: {inp['in_shapes']}. Out: {inp['out_shapes'] or 'none'}.", ""]
    if inp["out_examples"]:
        lines += ["Out examples: " + "; ".join(f"frame {e['frame']} `{e['hex']}`" for e in inp["out_examples"][:6]), ""]
    shim = result["shim"]
    if shim["files"]:
        lines += ["## Shim log", "", f"Files: {', '.join(shim['files'])}", ""]
        for name, n in sorted(shim["calls"].items()):
            lines.append(f"- {name} x{n} (first {shim['first'].get(name)})")
        lines.append("")
    if result["notes"]:
        lines += ["## Notes", ""] + [f"- {n['local']} {n['kind']} {n['text']}" for n in result["notes"]] + [""]
    return "\n".join(lines) + "\n"


def _jsonable(o):
    if isinstance(o, bytes):
        return o.hex(" ")
    if isinstance(o, Counter):
        return dict(o)
    raise TypeError(type(o).__name__)


def decode(path: Path, repo: Path | None = None, address: int | None = None) -> list[dict]:
    """Decodes a session folder, or every session folder (S*) in a day folder; writes the files. Results back."""
    sessions = [path] if any(p.suffix.lower() in CAPTURE_SUFFIXES for p in path.iterdir()) else \
        sorted(p for p in path.iterdir() if p.is_dir() and re.match(r"S[1-5]", p.name, re.IGNORECASE))
    results = []
    for folder in sessions:
        result = analyse_session(folder, repo, address)
        results.append(result)
        (folder / "analysis.json").write_text(json.dumps(result, indent=1, default=_jsonable), encoding="utf-8")
    # The feature map from any session (S1 has G HUB's discovery) names the HID++ features in the others.
    merged: dict[int, int] = {}
    for r in results:
        for k, v in r.get("hidpp", {}).get("feature_map", {}).items():
            merged.setdefault(int(k), v)
    for folder, r in zip(sessions, results):
        (folder / "summary.md").write_text(summary_markdown(r, merged), encoding="utf-8")
    if len(sessions) > 1 or path not in sessions:
        (path / "report.md").write_text(day_report(results, merged), encoding="utf-8")
    return results


def day_report(results: list[dict], feature_map: dict[int, int]) -> str:
    lines = ["# G923 capture report", ""]
    for r in results:
        cs = checks(r)
        bad = sum(1 for ok, _ in cs if not ok)
        lines.append(f"- **{r['session']}**: {'all checks pass' if not bad else f'{bad} check(s) failed'} "
                     f"(see its summary.md)")
    lines.append("")
    # Parameters across sessions: one table, with disagreements shown.
    params: dict[str, dict[str, float]] = defaultdict(dict)
    wb: dict[str, float] = {}
    for r in results:
        wb = wb or r.get("wheelbri_parameters", {})
        for k, p in r.get("truforce", {}).get("parameters", {}).items():
            params[k][r["session"]] = p["value"]
    if params:
        lines += ["## Parameters (all sessions)", "", "| Index | Real G923 | Sessions | WheelBri now |", "|---|---|---|---|"]
        for k in sorted(set(params) | set(wb), key=lambda s: int(s, 16)):
            seen = params.get(k, {})
            values = sorted(set(round(v, 6) for v in seen.values()))
            real = " / ".join(_fmt(v) for v in values) if values else "not asked"
            ours = wb.get(k, 0.0)
            differs = values and any(abs(v - ours) > 1e-4 * max(1, abs(v)) for v in values)
            lines.append(f"| {k} | {'**' if differs or len(values) > 1 else ''}{real}"
                         f"{'**' if differs or len(values) > 1 else ''} | {', '.join(seen) or '-'} | {_fmt(ours)} |")
        lines += ["", "Bold: differs from WheelBri, or differs between sessions.", ""]
    if feature_map:
        lines += ["## HID++ features", "", ", ".join(f"{i}: {hidpp.feature_name(f)}"
                                                     for i, f in sorted(feature_map.items())), ""]
    for r in results:
        lines += [f"## {r['session']}", ""]
        for ok, text in checks(r):
            lines.append(f"- {'PASS' if ok else '**FAIL**'}: {text}")
        lines.append("")
    return "\n".join(lines) + "\n"


# --- Locating the wheel ------------------------------------------------------------------------------------------------

def locate(paths: list[Path]) -> list[dict]:
    """For short captures of each root hub: where the G923 is (file, bus, address)."""
    found = []
    for p in paths:
        try:
            devs = find_devices(transfers(read_urbs(str(p))))
        except Exception as e:   # a hub that would not capture
            found.append({"file": str(p), "error": str(e)})
            continue
        for d in devs.values():
            if d.descriptor and is_target(d.descriptor.vendor, d.descriptor.product):
                found.append({"file": str(p), "bus": d.bus, "address": d.address,
                              "product": f"{d.descriptor.product:04X}"})
    return found


def keep_wheel_only(folder: Path) -> list[str]:
    """Rewrites each raw-*.pcap in a session folder as capture[-N].pcap with only the G923's frames, then deletes the
    raw file: another device on the same hub (a keyboard) must not stay in the capture. A raw file without the wheel
    is deleted, but only when another raw file has it: if none has, nothing is deleted (better a capture to look at
    by hand than none). Lines saying what was done come back."""
    from .pcap import LINKTYPE_USBPCAP, parse_urb, read_frames, write_pcap
    said: list[str] = []
    raws = sorted(p for p in folder.glob("raw-*.pcap*"))
    if not raws:
        return ["no raw-*.pcap here"]
    # Every (bus, address) the wheel had in a file: it can enumerate again in a session (a reset, a mode switch, G HUB),
    # and each of its addresses is kept.
    wheels: dict[Path, tuple[set[tuple[int, int]], str]] = {}
    for raw in raws:
        try:
            devs = find_devices(transfers(read_urbs(str(raw))))
            dev, why = pick_target(devs)
        except Exception as e:   # noqa: BLE001 - a broken file is reported, not fatal
            said.append(f"could not read {raw.name}: {e}")
            continue
        if dev is not None:
            keys = {(dev.bus, dev.address)}
            keys |= {(d.bus, d.address) for d in devs.values()
                     if d.descriptor and is_target(d.descriptor.vendor, d.descriptor.product)}
            # The G923 in Xbox mode (before G HUB switches it): a Logitech device speaking Xbox's GIP (an interface of
            # class FF, subclass 47, protocol D0), whatever its product ID.
            if rehearsal_target() is None:
                keys |= {(d.bus, d.address) for d in devs.values()
                         if d.descriptor and d.descriptor.vendor == VENDOR_ID
                         and any((i.cls, i.subclass, i.protocol) == (0xFF, 0x47, 0xD0) for i in d.interfaces)}
            wheels[raw] = (keys, why)
    if not wheels:
        said.append("no G923 found in any raw capture: all kept as they are (look in Wireshark; they may hold other "
                    "devices' input, so delete them when done)")
        return said
    n = 0
    for raw in raws:
        if raw not in wheels:
            raw.unlink()
            said.append(f"deleted {raw.name}: no G923 in it")
            continue
        keys, why = wheels[raw]
        frames = []
        for ts, linktype, frame in read_frames(str(raw)):
            if linktype != LINKTYPE_USBPCAP:
                continue
            u = parse_urb(ts, frame)
            if u is not None and (u.bus, u.device) in keys:
                frames.append((ts, frame))
        name = "capture.pcap" if n == 0 else f"capture-{n + 1}.pcap"
        n += 1
        write_pcap(str(folder / name), frames)
        raw.unlink()
        where = ", ".join(f"bus {b} address {a}" for b, a in sorted(keys))
        said.append(f"kept {len(frames)} frames of the wheel ({where}; found by {why}) from {raw.name} in {name}")
    return said
