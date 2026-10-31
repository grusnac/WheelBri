"""python -m g923cap decode <folder> [--repo R] [--address N] | locate <pcap...> | probe --out <folder> [--repo R]"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import analyse, probe


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="g923cap")
    sub = p.add_subparsers(dest="command", required=True)
    d = sub.add_parser("decode", help="decode a session folder or a day folder")
    d.add_argument("folder", type=Path)
    d.add_argument("--repo", type=Path, default=None, help="WheelBri's root, to compare with its values")
    d.add_argument("--address", type=int, default=None, help="the wheel's USB address, if not found by itself")
    d.add_argument("--quiet", action="store_true")
    loc = sub.add_parser("locate", help="which of these short captures has the G923")
    loc.add_argument("captures", type=Path, nargs="+")
    k = sub.add_parser("keep", help="keep only the wheel's frames in a session's raw captures (privacy, size)")
    k.add_argument("folder", type=Path)
    pr = sub.add_parser("probe", help="read-only questions to the wheel (Windows)")
    pr.add_argument("--out", type=Path, required=True)
    pr.add_argument("--repo", type=Path, default=None)
    pr.add_argument("--ghub", default="unknown", help="whether G HUB is running (for the file name)")
    a = p.parse_args(argv)

    if a.command == "decode":
        if not a.folder.is_dir():
            print(f"no folder {a.folder}")
            return 2
        results = analyse.decode(a.folder, a.repo, a.address)
        failed = 0
        for r in results:
            if not a.quiet:
                print(f"== {r['session']}")
            for ok, text in analyse.checks(r):
                failed += not ok
                if not a.quiet:
                    print(f"  {'PASS' if ok else 'FAIL'}  {text}")
        if not results:
            print("no sessions found")
            return 2
        return 1 if failed else 0
    if a.command == "locate":
        found = analyse.locate(a.captures)
        print(json.dumps(found))
        return 0 if any("address" in f for f in found) else 1
    if a.command == "keep":
        kept = analyse.keep_wheel_only(a.folder)
        for line in kept:
            print(line)
        return 0 if any(line.startswith("kept") for line in kept) else 1
    if a.command == "probe":
        return probe.run(a.out, a.repo, a.ghub)
    return 2


if __name__ == "__main__":
    sys.exit(main())
