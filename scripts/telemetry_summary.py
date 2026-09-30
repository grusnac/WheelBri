#!/usr/bin/env python3
"""Summarise a wheelemu telemetry session (standard library only).

usage: python scripts/telemetry_summary.py [telemetry-YYYYMMDD-HHMMSS.csv | folder]

With a folder (default: the service's log folder) the newest session is used. The matching
ffb-*.csv next to it is read too.
"""

import collections
import csv
import os
import statistics
import sys


def default_folder():
    if os.name == "nt":
        return os.path.join(os.environ.get("LOCALAPPDATA", ""), "wheelemu", "logs")
    state = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(state, "wheelemu")


def pick_session(arg):
    path = arg or default_folder()
    if os.path.isdir(path):
        sessions = sorted(f for f in os.listdir(path) if f.startswith("telemetry-") and f.endswith(".csv"))
        if not sessions:
            sys.exit(f"no telemetry-*.csv in {path}")
        path = os.path.join(path, sessions[-1])
    return path


def read_csv(path):
    comments, rows = [], []
    with open(path, newline="") as f:
        lines = []
        for line in f:
            (comments if line.startswith("#") else lines).append(line.rstrip("\n"))
    rows = list(csv.DictReader(lines))
    return comments, rows


def num(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def percentile(values, p):
    if not values:
        return float("nan")
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100.0 * len(values)))]


def main():
    path = pick_session(sys.argv[1] if len(sys.argv) > 1 else None)
    comments, rows = read_csv(path)
    print(f"session  {os.path.basename(path)}")
    for c in comments[1:]:
        if c.startswith(("# wheelemu-service", "# source:", "# target_profile:", "# config:")):
            print(f"         {c[2:]}")
    if not rows:
        print("no rows")
        return
    duration = num(rows[-1]["t_ms"]) / 1000.0
    print(f"rows     {len(rows)} over {duration:.1f} s")
    if any(c.startswith("# stopped") for c in comments):
        print("         NOTE: file hit max_file_mb, recording stopped early")

    print("\nphysical axes (raw -1..1): min / max  -> moved?")
    for i in range(8):
        values = [num(r[f"raw_a{i}"]) for r in rows]
        lo, hi = min(values), max(values)
        print(f"  a{i}  {lo:+.3f} / {hi:+.3f}  {'moved' if hi - lo > 0.05 else ''}")

    presses = collections.Counter()
    previous = 0
    for r in rows:
        buttons = int(r["raw_buttons"] or "0", 16)
        for bit in range(64):
            if buttons & (1 << bit) and not previous & (1 << bit):
                presses[bit] += 1
        previous = buttons
    print("\nphysical button presses (sampled): " +
          (", ".join(f"{b}x{n}" for b, n in sorted(presses.items())) or "none"))

    print("\nmapped pedals: share of time pressed (>5%) / max")
    for name in ("throttle", "brake", "clutch"):
        values = [num(r[name]) for r in rows]
        pressed = sum(1 for v in values if v > 0.05) / len(values)
        print(f"  {name:8s} {pressed:6.1%}  max {max(values):.3f}")
    both = sum(1 for r in rows if num(r["throttle"]) > 0.05 and num(r["brake"]) > 0.05) / len(rows)
    print(f"  throttle and brake together: {both:.1%}")
    steer = [num(r["steer"]) for r in rows]
    print(f"  steer    {min(steer):+.3f} .. {max(steer):+.3f}")

    print("\nsent to the game (emulated wheel units): min / max")
    for name in ("out_steer", "out_throttle", "out_brake", "out_clutch"):
        values = [num(r[name]) for r in rows]
        print(f"  {name:12s} {min(values):7.0f} / {max(values):7.0f}")

    if rows[-1]["ffb_packets"] == "":
        print("\nforce feedback: off")
    else:
        print(f"\nforce feedback: {rows[-1]['ffb_packets']} packets, {rows[-1]['ffb_plays']} wheel updates, "
              f"{rows[-1]['ffb_failures']} failed")
        ages = [num(r["ffb_age_ms"]) for r in rows if r["ffb_age_ms"] != ""]
        if ages:
            print(f"  packet age: median {statistics.median(ages):.0f} ms, max {max(ages):.0f} ms")
        watchdog = sum(1 for r in rows if r["ffb_watchdog"] == "1") / len(rows)
        print(f"  watchdog tripped in {watchdog:.1%} of rows (game quiet > 500 ms: menus, pause, alt-tab)")
        by_type = collections.defaultdict(lambda: {"rows": 0, "req": [], "out": []})
        for r in rows:
            for s in range(8):
                kind = r[f"s{s}_type"]
                if kind and "p" in r[f"s{s}_state"]:
                    entry = by_type[kind]
                    entry["rows"] += 1
                    entry["req"].append(abs(num(r[f"s{s}_req"]) or 0.0))
                    if r[f"s{s}_out"] != "":
                        entry["out"].append(abs(num(r[f"s{s}_out"])))
        print("  effect      playing  |req| p95 / max   |out| p95 / max")
        for kind, e in sorted(by_type.items()):
            print(f"  {kind:10s} {e['rows'] / len(rows):7.1%}   {percentile(e['req'], 95):.2f} / {max(e['req']):.2f}"
                  f"      {percentile(e['out'], 95):.2f} / {max(e['out'] or [0]):.2f}")

    ffb_path = os.path.join(os.path.dirname(path), os.path.basename(path).replace("telemetry-", "ffb-"))
    if os.path.exists(ffb_path):
        _, events = read_csv(ffb_path)
        kinds = collections.Counter(e["kind"] if e["kind"] != "slot" else f"slot:{e['type']}" for e in events)
        print(f"\nforce feedback changes from the game: {len(events)}  " +
              ", ".join(f"{k} {n}" for k, n in kinds.most_common()))

    periods = [num(r["period_us"]) for r in rows[1:]]
    work = [num(r["work_us"]) for r in rows[1:]]
    if periods:
        print(f"\nloop period us: median {statistics.median(periods):.0f}, p99 {percentile(periods, 99):.0f}, "
              f"max {max(periods):.0f}; work max {max(work):.0f} us")


if __name__ == "__main__":
    main()
