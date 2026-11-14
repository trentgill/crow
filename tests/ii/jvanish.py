"""Interruptions with Just Friends on the bus.

    python3 tests/ii/jvanish.py leader [N]   # crow A reset mid-traffic N times (default 10)
    python3 tests/ii/jvanish.py jf [N]       # JF reset mid-traffic N times

leader: crow A queries JF and is reset over its ST-Link at a random moment,
while crow B keeps querying JF throughout. A leader that vanishes part-way
through a transfer can leave JF holding SDA, or its peripheral BUSY.
jf: both crows query JF and JF is restarted (SYSRESETREQ over its ST-Link)
at a random moment; queries while it is gone are NACKed (nack_drop).
Every answer either crow gets must be right, and JF must answer again.
"""
import os, random, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.expanduser(os.environ.get("JF_SWD_DIR", "~/d/jf-ii")))
import c2c, crowhw, jfswd  # noqa: E402

STLINK = os.environ.get("CROW_STLINK", "066BFF383834434257094241")
RUN0 = 0x1800


def setup(A, B, raw0=RUN0):
    A.lua("JS={} ii.jf.event=function(e,v) JS[e.name]=v end ii.jf.get('speed') ii.jf.get('tsc')")
    time.sleep(0.2)
    g1 = {"A": int(float(A.lua("print(JS.speed)")[-1])), "B": int(float(A.lua("print(JS.tsc)")[-1]))}
    c2c.load(A, 1, 2)  # put back the script's ii.jf.event
    v = (raw0 + 0.5) / 1638.3
    A.lua("ii.jf.run(%r) ii.jf.transpose(%r)" % (v, v))
    time.sleep(0.2)
    return g1


def start(c, who, g1, n=100000, period=0.001, per=2, raw0=RUN0):
    c.write("runj(%d,%g,%d,'jq',%d,%d,'safe','%s')\r\n" % (n, period, per, g1[who], raw0, who))


def leader(n):
    A, B = c2c.open_crow(c2c.A_SN), c2c.open_crow(c2c.B_SN)
    c2c.load(A, 1, 2)
    c2c.load(B, 2, 1)
    g1 = setup(A, B)
    jf = jfswd.JF()
    t0 = jf.read()
    B.lua("ii.stats('reset')")
    start(B, "B", g1, period=0.002, per=1)
    dfu = 0
    for i in range(n):
        c2c.load(A, 1, 2)
        A.lua("ii.stats('reset')")
        start(A, "A", g1)
        time.sleep(random.uniform(0.3, 1.0))
        A.close()
        subprocess.run(["st-flash", "--serial", STLINK, "reset"], capture_output=True, text=True)
        A = crowhw.Crow(usb_serial=c2c.A_SN, stlink=STLINK, settle=0.3)
        try:
            A.wait_port(gone_first=True, timeout=4)
        except TimeoutError:
            dfu += 1  # booted into its bootloader: SCL was low at the instant it looked
            print(i, "A started its bootloader; leaving it", flush=True)
            subprocess.run([sys.executable, os.path.join(HERE, "dfuse.py"), "leave"],
                           env=dict(os.environ, DFU_BUS="7"), capture_output=True)
            A.wait_port(timeout=15)
        A.open()
        A._boot_text()
        print(i, "A back:", A.command("^^v", idle=0.3)[:1], flush=True)
    time.sleep(3)
    d = c2c.parse(B.lua("reportj()", timeout=5))
    j = jfswd.diff(t0, jf.read())
    jf.close()
    print("A entered its bootloader", dfu, "of", n, "resets")
    print("B: queries", d.get("J.qsent"), "ok", d.get("J.ok"), "wrong", d.get("J.bad"), "q_full", d.get("q_full"),
          "nack_drop", d.get("nack_drop"), "abandon", d.get("abandon"), "recover", d.get("recover"),
          "busy_max", d.get("busy_max_us"))
    print("JF:", j)
    return d, j


def jfreset(n):
    A, B = c2c.open_crow(c2c.A_SN), c2c.open_crow(c2c.B_SN)
    c2c.load(A, 1, 2)
    c2c.load(B, 2, 1)
    g1 = setup(A, B, raw0=0)  # JF forgets run and transpose when it restarts: use what it starts with
    jf = jfswd.JF()
    for c in (A, B):
        c.lua("ii.stats('reset')")
    start(A, "A", g1, period=0.002, per=1, raw0=0)
    start(B, "B", g1, period=0.002, per=1, raw0=0)
    for i in range(n):
        time.sleep(random.uniform(0.5, 1.5))
        jf.reset_core()
        time.sleep(0.6)  # JF's bootloader, then its app, then it waits for the bus to be free
        print(i, "JF back:", {k: v for k, v in jf.read().items() if k in ("addr_rx", "addr_tx", "getters", "recover", "berr")}, flush=True)
    time.sleep(3)
    for c in (A, B):
        c.lua("metro.free(MJ.id)")
    time.sleep(0.2)
    out = {}
    for who, c in (("A", A), ("B", B)):
        d = c2c.parse(c.lua("reportj()", timeout=5))
        out[who] = d
        print(who, ": queries", d.get("J.qsent"), "ok", d.get("J.ok"), "wrong", d.get("J.bad"), "q_full", d.get("q_full"),
              "nack_drop", d.get("nack_drop"), "abandon", d.get("abandon"), "recover", d.get("recover"),
              "scl_held", d.get("scl_held"), "sda_held", d.get("sda_held"), "busy_max", d.get("busy_max_us"))
        print("   wrong answers:", d.get("J.bads"))
    # and JF answers properly afterwards
    for who, c in (("A", A), ("B", B)):
        c.lua("ii.stats('reset')")
        start(c, who, g1, n=500, period=0.002, per=1, raw0=0)
    time.sleep(1.5)
    for who, c in (("A", A), ("B", B)):
        d = c2c.parse(c.lua("reportj()", timeout=5))
        print("after,", who, ": queries", d.get("J.qsent"), "ok", d.get("J.ok"), "wrong", d.get("J.bad"),
              "dropped", d.get("q_full", 0) + d.get("nack_drop", 0) + d.get("abandon", 0))
    print("JF:", {k: v for k, v in jf.read().items() if v and k not in ("magic", "exec") and not k.startswith("spare")})
    jf.close()
    return out


if __name__ == "__main__":
    what = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    {"leader": leader, "jf": jfreset}[what](n)
