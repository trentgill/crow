"""Crows leading at W/ (Tape engine, ii 0x71) on the same bus.

    python3 tests/ii/wslash.py --label w1 [--case NAME ...] [--repeat N]

Runs on whatever firmware W/ has: everything is checked from the crows. Each
answer W/ gives to a speed query is checked against the speed it should have.
A case can also keep crow-to-crow traffic going at the same time.
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c2c  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
# name: (crows querying W/, ticks, period, per tick, kind, crow-to-crow kind or None)
CASES = {
    "w_query":        ("A",  2000, 0.002, 1, "wq",   None),
    "w_query_fast":   ("A",  2000, 0.001, 2, "wq",   None),
    "w_setget":       ("A",  1000, 0.004, 1, "wset", None),
    "w_both_query":   ("AB", 2000, 0.002, 1, "wq",   None),
    "w_both_fast":    ("AB", 2000, 0.001, 2, "wq",   None),
    "w_query_plus_c2c": ("A", 2000, 0.002, 1, "wq",  "call"),   # B writes to A meanwhile
    "w_setget_plus_c2c": ("A", 1000, 0.004, 1, "wset", "mix"),  # B writes and queries A
    # getters W/ has no answer for, between speed queries: on W/'s original
    # driver the first one holds SCL low and stops the whole bus
    "w_badget":       ("A",   200, 0.005, 1, "wbad", None),
}


def run(A, B, name, spec):
    leaders, n, period, per, kind, c2ckind = spec
    for c in (A, B):
        c.lua("ii.fastmode(0) rxreset() ii.stats('reset') T={} W={}")
    # the speed W/ should report while nobody sets it
    want = None
    if kind in ("wq", "wbad"):
        A.lua("ii.wtape.speed(1)")
        time.sleep(0.2)
        want = 1.0
    if c2ckind:
        B.write("run(%d,%g,%d,'%s')\r\n" % (n, period, 1, c2ckind))
    for who, c in (("A", A), ("B", B)):
        if who in leaders:
            c.write("runw(%d,%g,%d,'%s',%s)\r\n" % (n, period, per, kind, "nil" if want is None else want))
    time.sleep(n * period + 2.0)
    res = {"case": name, "leaders": leaders, "kind": kind, "c2c": c2ckind}
    for who, c in (("A", A), ("B", B)):
        res[who] = c2c.parse(c.lua("reportw()", timeout=5))
        res[who + "c"] = c2c.parse(c.lua("report()", timeout=5))
    ok = True
    for who in leaders:
        d = res[who]
        answered = d.get("W.ok", 0) + d.get("W.bad", 0)
        dropped = d.get("q_full", 0) + d.get("abandon", 0) + d.get("nack_drop", 0)
        if d.get("W.bad", 0) or d.get("recover", 0) or d.get("scl_held", 0):
            ok = False
        if answered + dropped < d.get("W.qsent", 0) + d.get("W.sent", 0) - d.get("W.sent", 0) and not c2ckind:
            ok = False
    if c2ckind:  # B -> A crow traffic must be intact too
        L, F = res["Bc"], res["Ac"]
        if F.get("R.bad", 0) or F.get("R.dup", 0) or L.get("T.qbad", 0):
            ok = False
    res["pass"] = ok
    return res


def summary(r):
    s = []
    for who in ("A", "B"):
        d = r[who]
        if who in r["leaders"]:
            s.append("%s->W/: queries %d answered ok %d wrong %d (last wrong %s) | sets %d" % (
                who, d.get("W.qsent", 0), d.get("W.ok", 0), d.get("W.bad", 0), d.get("W.last_bad"), d.get("W.sent", 0)))
        errs = {k: v for k, v in d.items() if not k.startswith(("T.", "R.", "W.", "stuck_")) and v and k not in (
            "lead_tx", "lead_rx", "lead_tx_done", "lead_rx_done", "follow_addr_rx", "follow_addr_tx",
            "follow_rx_done", "follow_tx_done", "busy_max_us", "follow_defer", "follow_pad",
            "isr", "cr1", "cr2", "hal_state", "hal_mode", "hal_error", "op", "scl", "sda")}
        s.append("  %s: busy_max %dus %s" % (who, d.get("busy_max_us", 0), errs or "no errors"))
    if r["c2c"]:
        L, F = r["Bc"], r["Ac"]
        s.append("  B->A crow traffic: sent %d+%dq, A got %d (gap %d bad %d), queries ok %d bad %d" % (
            L.get("T.sent", 0), L.get("T.qsent", 0), F.get("R.n", 0), F.get("R.gap", 0), F.get("R.bad", 0),
            L.get("T.qok", 0), L.get("T.qbad", 0)))
    return "\n".join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--case", action="append")
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args()
    A, B = c2c.open_crow(c2c.A_SN), c2c.open_crow(c2c.B_SN)
    c2c.load(A, 1, 2)
    c2c.load(B, 2, 1)
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    out = open(os.path.join(HERE, "results", args.label + ".jsonl"), "a")
    fails = 0
    for _ in range(args.repeat):
        for name in args.case or list(CASES):
            r = run(A, B, name, CASES[name])
            fails += not r["pass"]
            out.write(json.dumps(r) + "\n")
            out.flush()
            print("%s %s" % ("PASS" if r["pass"] else "FAIL", name))
            print(summary(r))
    print("%d failed" % fails)


if __name__ == "__main__":
    main()
