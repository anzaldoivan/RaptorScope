#!/usr/bin/env python3
"""Validate dc1_aot_corpus TSV from the six pristine regional installs.

Usage: dc1_aot_corpus paths.txt | python tests/check_dc1_aot_corpus.py -
"""
import csv
import sys


REGIONS = ("japanese", "english", "french", "german", "italian", "spanish")
EXPECTED = {
    "japanese": "0:456,1:538,2:60,3:369,4:251,7:43,8:26,10:18,11:68",
    "english":  "0:456,1:538,2:60,3:369,4:251,7:43,8:26,10:18,11:68",
    "french":   "0:456,1:538,2:58,3:368,4:251,7:43,8:26,10:18,11:68",
    "german":   "0:456,1:538,2:58,3:368,4:251,7:43,8:26,10:18,11:68",
    "italian":  "0:456,1:538,2:58,3:368,4:251,7:43,8:26,10:18,11:68",
    "spanish":  "0:456,1:538,2:58,3:368,4:251,7:43,8:26,10:18,11:68",
}


def counts(text):
    return {int(k): int(v) for k, v in (part.split(":") for part in text.split(",") if part)}


def region(path):
    parts = path.replace("\\", "/").lower().split("/")
    return next((name for name in REGIONS if name in parts), None)


def filename(path):
    return path.replace("\\", "/").rsplit("/", 1)[-1].lower()


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    stream = sys.stdin if sys.argv[1] == "-" else open(sys.argv[1], encoding="utf-8", newline="")
    try:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    finally:
        if stream is not sys.stdin:
            stream.close()

    failures = []
    warnings = []
    for row in rows:
        if int(row["malformed"]):
            failures.append(f'{row["path"]}: malformed AOT')
        if int(row["overflow"]):
            failures.append(f'{row["path"]}: AOT capacity overflow')

    # Snapshot checks against the six pristine installs only warn.
    builds = 0
    for name in REGIONS:
        group = [row for row in rows if region(row["path"]) == name]
        if not group:
            warnings.append(f"{name}: no rows")
            continue
        builds += 1
        if len(group) != 148:
            warnings.append(f"{name}: expected 148 room RDTs, got {len(group)}")
        totals = {}
        for row in group:
            for action, n in counts(row["types"]).items():
                totals[action] = totals.get(action, 0) + n
        if totals != counts(EXPECTED.get(name, "")):
            warnings.append(f"{name}: subtype snapshot differs: {totals}")
        if max(int(row["raw_aots"]) for row in group) != 57:
            warnings.append(f"{name}: raw AOT maximum is not 57")
        if max(int(row["retained_aots"]) for row in group) != 44:
            warnings.append(f"{name}: retained AOT maximum is not 44")

        st30c = [row for row in group if filename(row["path"]) == "st30c.dat"]
        expected_type2 = 3 if name in ("japanese", "english") else 1
        if len(st30c) != 1 or counts(st30c[0]["types"]).get(2) != expected_type2:
            warnings.append(f"{name}: ST30C action-2 checkpoint differs")

    if warnings:
        print("\n".join("WARN " + warning for warning in warnings))
    if failures:
        print("\n".join("FAIL " + failure for failure in failures))
        return 1
    print(f"PASS {builds} builds, {len(rows)} room RDTs; per-room AOT invariants hold")
    return 0

if __name__ == "__main__":
    sys.exit(main())
