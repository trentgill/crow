"""crow-to-crow ii stress runner.

    python3 tests/ii/c2c.py --label base [--case NAME ...]

crow A (address 1) and crow B (address 2) are found by USB serial number
(CROW_A / CROW_B, defaults are the two bench units). Each case loads c2c.lua on
both, starts traffic on one or both leaders, waits, then reads ii.stats() and
the script's own counts from both sides. One JSON line per case goes to
tests/ii/results/<label>.jsonl.
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "hw"))
import crowhw  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
A_SN = os.environ.get("CROW_A", "206135773756")
B_SN = os.environ.get("CROW_B", "206635743756")

# name: (leaders, ticks, period s, per tick, kind, timing)
CASES = {
    "a2b_call_slow":   ("A",  2000, 0.002, 1, "call",  0),
    "a2b_call_fast":   ("A",  2000, 0.001, 4, "call",  0),
    "a2b_query":       ("A",  2000, 0.002, 1, "query", 0),
    "a2b_mix":         ("A",  2000, 0.001, 2, "mix",   0),
    "b2a_call_fast":   ("B",  2000, 0.001, 4, "call",  0),
    "b2a_mix":         ("B",  2000, 0.001, 2, "mix",   0),
    "a2b_call_fastmode": ("A", 2000, 0.001, 4, "call", 1),
    "a2b_mix_fastmode":  ("A", 2000, 0.001, 2, "mix",  1),
    "a2b_mix_classic":   ("A", 2000, 0.001, 2, "mix",  "'classic'"),
    "both_call":       ("AB", 2000, 0.002, 1, "call",  0),
    "both_mix":        ("AB", 2000, 0.001, 2, "mix",   0),
    "both_call_fast":  ("AB", 2000, 0.001, 4, "call",  0),
    # kind "x/y": A sends x, B sends y
    "both_query_slow": ("AB", 1000, 0.005, 1, "query", 0),
    "aq_bc_slow":      ("AB", 1000, 0.005, 1, "query/call", 0),
    "ac_bq_slow":      ("AB", 1000, 0.005, 1, "call/query", 0),
    "both_call_slow":  ("AB", 1000, 0.005, 1, "call", 0),
}


def parse(lines):
    for l in lines:
        if l.startswith("REPORT "):
            d = {}
            for kv in l[7:].split():
                k, v = kv.split("=", 1)
                try:
                    d[k] = float(v) if "." in v else int(v)
                except ValueError:
                    d[k] = {"true": True, "false": False}.get(v, v)
            return d
    raise RuntimeError("no REPORT in %r" % lines[-5:])


def open_crow(sn):
    c = crowhw.Crow(usb_serial=sn, settle=0.3).open()
    return c


def load(c, me, other):
    script = open(os.path.join(HERE, "c2c.lua")).read()
    _, lines, ok = c.upload(script, end="^^e")
    if not ok:
        raise RuntimeError("upload failed: %r" % lines[-5:])
    c.lua("setup(%d,%d)" % (me, other))


def run_case(A, B, name, spec, wait_extra=2.0):
    leaders, n, period, per, kind, timing = spec
    for c in (A, B):
        c.lua("ii.fastmode(%s)" % timing)
        c.lua("rxreset() ii.stats('reset') T={}")
    kinds = dict(zip("AB", kind.split("/"))) if "/" in kind else {"A": kind, "B": kind}
    for who, c in (("A", A), ("B", B)):
        if who in leaders:
            c.write("run(%d,%g,%d,'%s')\r\n" % (n, period, per, kinds[who]))
    t0 = time.monotonic()
    time.sleep(n * period + wait_extra)
    res = {"case": name, "leaders": leaders, "n": n, "period": period, "per": per,
           "kind": kind, "timing": str(timing), "secs": round(time.monotonic() - t0, 2)}
    for who, c in (("A", A), ("B", B)):
        out = c.lua("report()", timeout=5)
        res[who] = parse(out)
        res[who + "_text"] = [l for l in out if not l.startswith("REPORT ")][-10:]
    return res


def summary(r):
    """The few numbers that say whether a case passed."""
    s = []
    for who, other in (("A", "B"), ("B", "A")):
        if who not in r["leaders"]:
            continue
        L, F = r[who], r[other]
        sent, qsent = L.get("T.sent", 0), L.get("T.qsent", 0)
        s.append("%s->%s: call sent %d got %d (gap %d dup %d bad %d ooo %d) | query sent %d ok %d bad %d"
                 % (who, other, sent, F.get("R.n", 0), F.get("R.gap", 0), F.get("R.dup", 0),
                    F.get("R.bad", 0), F.get("R.ooo", 0), qsent, L.get("T.qok", 0), L.get("T.qbad", 0)))
    for who in ("A", "B"):
        d = r[who]
        errs = {k: v for k, v in d.items() if not k.startswith(("T.", "R.")) and v and k not in (
            "lead_tx", "lead_rx", "lead_tx_done", "lead_rx_done", "follow_addr_rx", "follow_addr_tx",
            "follow_rx_done", "follow_tx_done", "busy_max_us",
            "isr", "cr1", "cr2", "hal_state", "hal_mode", "hal_error", "op", "scl", "sda")}
        s.append("  %s: busy_max %dus  %s" % (who, d.get("busy_max_us", 0), errs or "no errors"))
        if d.get("op") != 2 or not (d.get("scl") and d.get("sda")):
            s.append("     STUCK: op %s hal_state 0x%x mode 0x%x err 0x%x isr 0x%x cr1 0x%x cr2 0x%x scl %s sda %s" % (
                d.get("op"), d.get("hal_state", 0), d.get("hal_mode", 0), d.get("hal_error", 0),
                d.get("isr", 0), d.get("cr1", 0), d.get("cr2", 0), d.get("scl"), d.get("sda")))
    return "\n".join(s)


def passed(r):
    for who, other in (("A", "B"), ("B", "A")):
        if who not in r["leaders"]:
            continue
        L, F = r[who], r[other]
        if F.get("R.n", 0) != L.get("T.sent", 0) or L.get("T.qok", 0) != L.get("T.qsent", 0):
            return False
        if F.get("R.gap", 0) or F.get("R.dup", 0) or F.get("R.bad", 0) or F.get("R.ooo", 0):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--case", action="append")
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args()
    names = args.case or list(CASES)
    A, B = open_crow(A_SN), open_crow(B_SN)
    load(A, 1, 2)
    load(B, 2, 1)
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    out = open(os.path.join(HERE, "results", args.label + ".jsonl"), "a")
    fails = 0
    for _ in range(args.repeat):
        for name in names:
            try:
                r = run_case(A, B, name, CASES[name])
            except Exception as e:  # a crow stopped answering over USB
                print("FAIL %s: %s" % (name, e))
                fails += 1
                break
            r["pass"] = passed(r)
            fails += not r["pass"]
            out.write(json.dumps(r) + "\n")
            out.flush()
            print("%s %s" % ("PASS" if r["pass"] else "FAIL", name))
            print(summary(r))
            for who in ("A", "B"):
                if r[who + "_text"]:
                    print("  %s said: %r" % (who, r[who + "_text"]))
    print("%d failed" % fails)


if __name__ == "__main__":
    main()
