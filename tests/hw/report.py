#!/usr/bin/env python3
"""Compare runs of the hardware suite.

    python3 tests/hw/report.py baseline a3 a1      # markdown on stdout

The first label is the reference: every other column shows its value and the
change against it, marked + when that change is in the wanted direction.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(label):
    return json.load(open(os.path.join(HERE, "results", label + ".json")))


def fmt(v):
    if v is None:
        return ""
    if isinstance(v, float) and v != int(v):
        return "%.2f" % v if abs(v) < 100 else "%.0f" % v
    return "{:,}".format(int(v))


def main():
    labels = sys.argv[1:]
    if not labels:
        sys.exit(__doc__)
    runs = [load(l) for l in labels]
    names = []
    for r in runs:
        names += [n for n in r["metrics"] if n not in names]

    print("| metric | unit | " + " | ".join(labels) + " |")
    print("| --- | --- | " + " | ".join("---:" for _ in labels) + " |")
    for n in names:
        ref = runs[0]["metrics"].get(n)
        unit = next(r["metrics"][n]["unit"] for r in runs if n in r["metrics"])
        cells = []
        for i, r in enumerate(runs):
            m = r["metrics"].get(n)
            if m is None:
                cells.append("")
                continue
            cell = fmt(m["value"])
            if i and ref and ref["value"] and m["better"]:
                pct = 100.0 * (m["value"] - ref["value"]) / abs(ref["value"])
                if abs(pct) >= 0.5:
                    good = (pct < 0) == (m["better"] == "lower")
                    cell += " (%s%.0f%% %s)" % ("+" if pct > 0 else "", pct, "better" if good else "worse")
            cells.append(cell)
        print("| %s | %s | %s |" % (n, unit, " | ".join(cells)))

    checks = []
    for r in runs:
        checks += [n for n in r["checks"] if n not in checks]
    print()
    print("| check | " + " | ".join(labels) + " |")
    print("| --- | " + " | ".join("---" for _ in labels) + " |")
    for n in checks:
        cells = []
        for r in runs:
            k = r["checks"].get(n)
            cells.append("" if k is None else ("pass" if k["ok"] else "FAIL"))
        print("| %s | %s |" % (n, " | ".join(cells)))
    for r in runs:
        fails = [(n, k["detail"]) for n, k in r["checks"].items() if not k["ok"]]
        if fails:
            print("\n%s failing:" % r["label"])
            for n, d in fails:
                print("- %s: %s" % (n, d))


if __name__ == "__main__":
    main()
