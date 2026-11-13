"""Crows leading at Just Friends (ii 0x70) on the same bus.

    python3 tests/ii/jf.py --label j1 [--case NAME ...] [--repeat N] [--all-values]

Every answer JF gives is checked on the crows against what it should be. When
JF's telemetry build is on it (JF_ELF, default ~/d/JF3-ii/JF.elf), its own
counters are read over SWD around each case through ~/d/jf-ii/jfswd.py, so a
setter JF ran that nobody sent, a dropped setter or a bus error shows up too.
"""
import argparse, json, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.expanduser(os.environ.get("JF_SWD_DIR", "~/d/jf-ii")))
import c2c  # noqa: E402

# name: (crows leading at JF, ticks, period, per tick, kind, crow-to-crow kind or None)
CASES = {
    "j_query":        ("A",  2000, 0.002, 1, "jq",    None),
    "j_query_fast":   ("A",  2000, 0.001, 2, "jq",    None),
    "j_setget":       ("A",  1000, 0.004, 1, "jset",  None),
    "j_setget_fast":  ("A",  2000, 0.001, 1, "jset",  None),
    "j_note":         ("A",  2000, 0.001, 2, "jnote", None),
    "j_vox":          ("A",  2000, 0.001, 2, "jvox",  None),
    "j_long":         ("A",  1000, 0.004, 1, "jlong", None),
    "j_badget":       ("A",   400, 0.005, 1, "jbad",  None),
    "j_both_query":   ("AB", 2000, 0.002, 1, "jq",    None),
    "j_both_fast":    ("AB", 2000, 0.001, 2, "jq",    None),
    "j_both_note":    ("AB", 2000, 0.001, 2, "jnote", None),
    "j_both_note_slow": ("AB", 2000, 0.002, 1, "jnote", None),
    "j_both_setget":  ("AB", 1000, 0.004, 1, "jset",  None),
    "j_both_badget":  ("AB",  400, 0.005, 1, "jbad",  None),
    "j_query_plus_c2c": ("A", 2000, 0.002, 1, "jq",   "call"),   # B writes to A meanwhile
    "j_setget_plus_c2c": ("A", 1000, 0.004, 1, "jset", "mix"),   # B writes and queries A
    # crow's fast bus timing (ii.fastmode(1), ~400 kHz)
    "j_query_fastmode":  ("A",  2000, 0.001, 2, "jq",    None, 1),
    "j_setget_fastmode": ("A",  2000, 0.001, 1, "jset",  None, 1),
    "j_note_fastmode":   ("A",  2000, 0.001, 2, "jnote", None, 1),
    "j_both_fastmode":   ("AB", 2000, 0.001, 2, "jq",    None, 1),
}
RUN0 = 0x1800  # run value set before a case: high byte outside JF's command range


def jf_open():
    if os.environ.get("JF_NOSWD"):
        return None
    try:
        import jfswd
        return jfswd.JF(os.path.expanduser(os.environ.get("JF_ELF", "~/d/JF3-ii/JF.elf")))
    except Exception as e:
        print("no JF telemetry: %s" % e)
        return None


def run(A, B, jf, name, spec, vals, g1):
    leaders, n, period, per, kind, c2ckind = spec[:6]
    timing = spec[6] if len(spec) > 6 else 0
    for c in (A, B):
        c.lua("ii.fastmode(%s) rxreset() ii.stats('reset') T={} J={}" % timing)
    A.lua("ii.jf.run(%r) ii.jf.transpose(%r)" % ((RUN0 + 0.5) / 1638.3, (RUN0 + 0.5) / 1638.3))
    time.sleep(0.2)
    t0 = jf.read() if jf else None
    if c2ckind:
        B.write("run(%d,%g,%d,'%s')\r\n" % (n, period, 1, c2ckind))
    for who, c in (("A", A), ("B", B)):
        if who in leaders:
            c.write("runj(%d,%g,%d,'%s',%d,%d,'%s','%s')\r\n" % (n, period, per, kind, g1[who], RUN0, vals, who))
    time.sleep(n * period + 2.0)
    res = {"case": name, "leaders": leaders, "kind": kind, "c2c": c2ckind, "vals": vals, "timing": timing}
    for who, c in (("A", A), ("B", B)):
        res[who] = c2c.parse(c.lua("reportj()", timeout=5))
        res[who + "c"] = c2c.parse(c.lua("report()", timeout=5))
    if jf:
        import jfswd
        res["JF"] = jfswd.diff(t0, jf.read())
    res["pass"], res["why"] = judge(res)
    return res


def judge(r):
    why = []
    on_bus = 0  # what the crows put on the bus: every one must reach JF once
    for who in r["leaders"]:
        d = r[who]
        dropped = d.get("q_full", 0) + d.get("abandon", 0) + d.get("nack_drop", 0)
        if d.get("J.bad", 0):
            why.append("%s got %d wrong answers (last %s)" % (who, d["J.bad"], d.get("J.last_bad")))
        if d.get("recover", 0) or d.get("scl_held", 0):
            why.append("%s driver reset itself (recover %d scl_held %d)" % (who, d.get("recover", 0), d.get("scl_held", 0)))
        answered = d.get("J.ok", 0) + d.get("J.bad", 0)
        if answered + dropped < d.get("J.qsent", 0):
            why.append("%s: %d queries never answered" % (who, d.get("J.qsent", 0) - answered - dropped))
        # a crow drops what it can't queue (q_full) when the case asks for
        # more than the bus carries: counted, not a fault
        if d.get("abandon", 0) or d.get("nack_drop", 0):
            why.append("%s gave up on %d (abandon %d nack_drop %d)" % (
                who, d.get("abandon", 0) + d.get("nack_drop", 0), d.get("abandon", 0), d.get("nack_drop", 0)))
        on_bus += d.get("J.qsent", 0) + d.get("J.sent", 0) + d.get("J.nsent", 0) - dropped
    if r["c2c"]:  # B -> A crow traffic must be intact too
        L, F = r["Bc"], r["Ac"]
        if F.get("R.bad", 0) or F.get("R.dup", 0) or L.get("T.qbad", 0):
            why.append("crow-to-crow traffic damaged")
    j = r.get("JF")
    if j:
        seen = j.get("setters", 0) + j.get("getters", 0)
        if seen != on_bus:
            why.append("JF took %d setters + %d queries, crows put %d on the bus" % (
                j.get("setters", 0), j.get("getters", 0), on_bus))
        for k in ("berr", "ovr", "q_full", "tx_qfull", "recover"):
            if j.get(k):
                why.append("JF %s %d" % (k, j[k]))
        if j.get("rx_ovf") and r["kind"] != "jlong":
            why.append("JF rx_ovf %d" % j["rx_ovf"])
        if j.get("tx_empty") and r["kind"] != "jbad":
            why.append("JF tx_empty %d" % j["tx_empty"])
    return not why, why


def summary(r):
    s = []
    for who in r["leaders"]:
        d = r[who]
        s.append("%s->JF: queries %d ok %d (late %d) wrong %d | setters %d notes %d | q_full %d | busy_max %dus" % (
            who, d.get("J.qsent", 0), d.get("J.ok", 0), d.get("J.late", 0), d.get("J.bad", 0),
            d.get("J.sent", 0), d.get("J.nsent", 0), d.get("q_full", 0), d.get("busy_max_us", 0)))
    if r["c2c"]:
        L, F = r["Bc"], r["Ac"]
        s.append("  B->A crow traffic: sent %d+%dq, A got %d (gap %d bad %d), queries ok %d bad %d" % (
            L.get("T.sent", 0), L.get("T.qsent", 0), F.get("R.n", 0), F.get("R.gap", 0), F.get("R.bad", 0),
            L.get("T.qok", 0), L.get("T.qbad", 0)))
    if r.get("JF"):
        s.append("  JF: %s" % r["JF"])
    for w in r["why"]:
        s.append("  ! " + w)
    return "\n".join(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--case", action="append")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--all-values", action="store_true",
                    help="set/answer every high byte, including JF's command numbers (fixed driver only)")
    args = ap.parse_args()
    A, B = c2c.open_crow(c2c.A_SN), c2c.open_crow(c2c.B_SN)
    c2c.load(A, 1, 2)
    c2c.load(B, 2, 1)
    jf = jf_open()
    # the switches JF reports: answers to compare against (nobody moves them)
    A.lua("JS={} ii.jf.event=function(e,v) JS[e.name]=v end ii.jf.get('speed') ii.jf.get('tsc')")
    time.sleep(0.2)
    g1 = {"A": int(float(A.lua("print(JS.speed)")[-1])), "B": int(float(A.lua("print(JS.tsc)")[-1]))}
    c2c.load(A, 1, 2)
    vals = "all" if args.all_values else "safe"
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    out = open(os.path.join(HERE, "results", args.label + ".jsonl"), "a")
    fails = 0
    try:
        for _ in range(args.repeat):
            for name in args.case or list(CASES):
                r = run(A, B, jf, name, CASES[name], vals, g1)
                fails += not r["pass"]
                out.write(json.dumps(r) + "\n")
                out.flush()
                print("%s %s" % ("PASS" if r["pass"] else "FAIL", name))
                print(summary(r), flush=True)
    finally:
        if jf:
            jf.close()
    print("%d failed" % fails)


if __name__ == "__main__":
    main()
