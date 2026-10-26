"""Turns ctest's JUnit file (ctest --output-junit) into the job's summary page: the totals, every failed test with the
end of its output, and each suite with its counts. Failed tests are also marked as errors in the log.

    python test-summary.py <results.xml> <title>

Writes to $GITHUB_STEP_SUMMARY (stdout when it is not set). Exit codes: 0 written, 1 the file is missing or not JUnit.
"""

import os
import sys
import xml.etree.ElementTree as ElementTree
from collections import defaultdict

KEEP_LINES = 30  # of a failed test's output


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    path, title = sys.argv[1], sys.argv[2]
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError) as error:
        print(f"::error::no test results ({path}): {error}")
        return 1

    suites = defaultdict(lambda: {"passed": 0, "failed": 0, "skipped": 0, "seconds": 0.0})
    failures = []
    for case in root.iter("testcase"):
        name = case.get("name", "?")
        suite = suites[name.split(".")[0].split("/")[-1]]  # Prefix/Suite.Test/0 -> Suite
        suite["seconds"] += float(case.get("time", "0") or 0)
        failure = case.find("failure")
        if failure is None:
            failure = case.find("error")
        if failure is not None or case.get("status") == "fail":
            suite["failed"] += 1
            output = (case.findtext("system-out") or "").strip().splitlines()
            failures.append((name, output[-KEEP_LINES:]))
        elif case.find("skipped") is not None or case.get("status") in ("notrun", "disabled", "skipped"):
            suite["skipped"] += 1
        else:
            suite["passed"] += 1

    passed = sum(s["passed"] for s in suites.values())
    failed = sum(s["failed"] for s in suites.values())
    skipped = sum(s["skipped"] for s in suites.values())
    seconds = sum(s["seconds"] for s in suites.values())

    lines = [f"## {'❌' if failed else '✅'} {title}", ""]
    lines.append(f"**{passed} passed**, **{failed} failed**, {skipped} skipped "
                 f"({passed + failed + skipped} tests in {len(suites)} suites, {seconds:.1f} s)")
    lines.append("")
    for name, output in failures:
        print(f"::error title=Test failed::{name}")
        lines += [f"<details open><summary>❌ <code>{name}</code></summary>", "", "```", *output, "```", "</details>", ""]
    lines += ["<details><summary>All suites</summary>", "", "| Suite | Passed | Failed | Skipped | Time |",
              "|---|---:|---:|---:|---:|"]
    for name in sorted(suites, key=lambda n: (-suites[n]["failed"], n)):
        s = suites[name]
        mark = "❌ " if s["failed"] else ""
        lines.append(f"| {mark}{name} | {s['passed']} | {s['failed']} | {s['skipped']} | {s['seconds']:.2f} s |")
    lines += ["", "</details>", ""]

    text = "\n".join(lines) + "\n"
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if target:
        with open(target, "a", encoding="utf-8") as summary:
            summary.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
