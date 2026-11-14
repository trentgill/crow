#!/usr/bin/env python3
"""Hardware test suite: measure a real crow and check it behaves.

    python3 tests/hw/run.py --label baseline              # test what is on the module
    python3 tests/hw/run.py --label a3 --flash crow.bin   # flash first (ST-Link), then test
    python3 tests/hw/run.py --label x --only cpu,lua      # a subset
    python3 tests/hw/report.py baseline a3                # compare two runs

Needs: the module on USB, an ST-Link on its SWD header (reset and recovery),
output 1 patched to input 1 and output 2 to input 2, and a firmware built with
TELEMETRY=1 (the default). Results go to tests/hw/results/<label>.json.

The suite overwrites the stored user script and leaves the module with none.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crowhw import Crow  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


class Results:
    def __init__(self, label):
        self.data = {"label": label, "date": time.strftime("%Y-%m-%d %H:%M:%S"),
                     "metrics": {}, "checks": {}}

    def metric(self, name, value, unit="", better=None):
        """better: 'lower', 'higher', or None when the number is only context."""
        self.data["metrics"][name] = {"value": value, "unit": unit, "better": better}
        print("    %-34s %s %s" % (name, _fmt(value), unit))

    def check(self, name, ok, detail=""):
        self.data["checks"][name] = {"ok": bool(ok), "detail": str(detail)}
        print("    %-34s %s  %s" % (name, "PASS" if ok else "FAIL", detail))


def _fmt(v):
    if isinstance(v, float):
        return "%.3f" % v if abs(v) < 100 else "%.1f" % v
    return str(v)


def fresh(c):
    """Back to a known state: reboot with whatever script is stored."""
    c.reset()


# --------------------------------------------------------------------------
# measurements

def t_build(c, R):
    binf = R.data.get("flashed") or os.path.join(ROOT, "crow.bin")
    if os.path.exists(binf):
        R.metric("flash_image", os.path.getsize(binf), "bytes", "lower")


def t_boot(c, R):
    dt = statistics.median(c.reset(settle=True) for _ in range(3))
    R.metric("boot_to_usb", dt * 1000, "ms", "lower")
    v = c.command("^^v")
    R.data["version"] = v[0] if v else "?"
    R.check("version_reply", bool(v) and v[0].startswith("^^version("), v[:1])
    R.check("identity_reply", any(l.startswith("^^identity(") for l in c.command("^^i")))
    # datasheet limits at 216MHz: voltage scale 1 (VOS=3), APB1 <= 54MHz, APB2 <= 108MHz
    t = c.telemetry()
    R.check("core_voltage_scale_in_spec", t["clock_hz"] <= 180e6 or t["vos"] == 3,
            "%.0f MHz at VOS=%d" % (t["clock_hz"] / 1e6, t["vos"]))
    R.check("bus_clocks_in_spec", t["pclk1_hz"] <= 54e6 and t["pclk2_hz"] <= 108e6,
            "APB1 %.0f MHz, APB2 %.0f MHz" % (t["pclk1_hz"] / 1e6, t["pclk2_hz"] / 1e6))


def t_ram(c, R):
    fresh(c)
    time.sleep(0.5)
    t = c.telemetry()
    R.metric("ram_static", int(t["ram_static"]), "bytes", "lower")
    R.metric("lua_heap_at_boot", int(t["lua_bytes"]), "bytes", "lower")
    R.metric("heap_claimed", int(t["heap_arena"]), "bytes", "lower")
    R.metric("stack_peak", int(t["stack_peak"]), "bytes")
    R.metric("ram_free_for_scripts", int(t["ram_gap"] + t["heap_free"]), "bytes", "higher")


def t_cpu(c, R):
    fresh(c)
    c.telemetry_reset()
    time.sleep(3.0)
    t = c.telemetry()
    R.metric("idle_cpu_busy", t["cpu_busy"] * 100, "%", "lower")
    R.metric("idle_dsp_block_avg", t["dsp_avg_us"], "us", "lower")
    R.metric("idle_dsp_block_max", t["dsp_max_us"], "us", "lower")
    R.metric("idle_dsp_load", t["dsp_load"] * 100, "%", "lower")
    R.metric("idle_loop_worst", t["loop_max_us"], "us", "lower")

    # a busy patch: four LFOs, a 200Hz input stream to the host, a change
    # detector and a 200Hz metro running Lua
    c.lua("for n=1,4 do output[n].action = lfo(0.02*n, 5) output[n]() end")
    c.lua("input[1].mode('stream', 0.005)")
    c.lua("input[2].mode('change', 1, 0.1, 'both')")
    c.lua("_x=0 _m = metro.init(function() _x = _x + 1 end, 0.005) _m:start()")
    time.sleep(0.3)
    c.telemetry_reset()
    c.read_lines(3.0)  # keep draining the stream
    t = c.telemetry()
    us = statistics.median(c.timed(BENCH["loop"]) for _ in range(5))
    R.metric("busy_cpu_busy", t["cpu_busy"] * 100, "%", "lower")
    R.metric("busy_dsp_block_avg", t["dsp_avg_us"], "us", "lower")
    R.metric("busy_dsp_block_max", t["dsp_max_us"], "us", "lower")
    R.metric("busy_loop_worst", t["loop_max_us"], "us", "lower")
    R.metric("busy_event_queue_peak", int(t["event_max"]), "of 40", "lower")
    R.metric("busy_lua_loop", us / 1000, "ms", "lower")
    R.check("busy_no_events_lost", t["event_full"] == 0, "%d dropped" % t["event_full"])
    R.check("busy_no_tx_dropped", t["tx_dropped"] == 0, "%d bytes" % t["tx_dropped"])
    c.events.clear()


# each stays well inside the watchdog's instruction budget and a few kB of heap
BENCH = {
    "loop":   "local x=0 for i=1,200000 do x=x+i*0.5 end",
    "call":   "local function f(a) return a+1 end local x=0 for i=1,100000 do x=f(x) end",
    "table":  "local t={} for r=1,15 do for i=1,2000 do t[i]=i+r end local s=0 for i=1,2000 do s=s+t[i] end end",
    "string": "for r=1,6 do local s={} for i=1,500 do s[#s+1]=tostring(i)..'x' end local r=table.concat(s) end",
    "math":   "local x=0 for i=1,40000 do x=x+math.sin(i*0.001) end",
    # runs crow's own library bytecode rather than code compiled from the chunk
    "library": "local s=sequins{1,2,sequins{3,4},5} local x=0 for i=1,6000 do x=x+s() end",
}


def t_lua(c, R):
    fresh(c)
    for name, body in BENCH.items():
        us = statistics.median(c.timed(body) for _ in range(7))
        R.metric("lua_" + name, us / 1000, "ms", "lower")


def t_latency(c, R):
    fresh(c)
    times = []
    for _ in range(200):
        t0 = time.perf_counter()
        c.write("print(1)\r\n")
        _, ok = c.read_lines(1.0, until="1")
        if ok:
            times.append((time.perf_counter() - t0) * 1000)
    times.sort()
    R.check("latency_all_answered", len(times) == 200, "%d of 200" % len(times))
    if times:
        R.metric("repl_roundtrip_median", statistics.median(times), "ms", "lower")
        R.metric("repl_roundtrip_p95", times[int(len(times) * 0.95) - 1], "ms", "lower")


def t_tx(c, R):
    """Device to host: print lines as fast as Lua can and see what arrives."""
    fresh(c)
    pad = "x" * 58
    for n in (20, 300):
        c.clear()
        c.telemetry_reset()
        t0 = time.perf_counter()
        c.write("for i=1,%d do print(string.format('L%%04d', i)..'%s') end\r\n" % (n, pad))
        lines, _ = c.read_lines(8.0, idle=0.6)
        dt = time.perf_counter() - t0 - 0.6
        good = [l for l in lines if re.fullmatch(r"L\d{4}x{58}", l)]
        seq = [int(l[1:5]) for l in good]
        whole = seq == list(range(1, n + 1))
        t = c.telemetry()
        R.metric("print_%d_lines_delivered" % n, 100.0 * len(set(seq)) / n, "%", "higher")
        R.check("print_%d_lines_whole" % n, whole,
                "%d intact of %d, %d bytes dropped by the device" % (len(good), n, t["tx_dropped"]))
        if n == 300:
            R.metric("print_throughput", sum(len(l) + 2 for l in lines) / max(dt, 1e-3) / 1000,
                     "kB/s", "higher")


def make_script(nbytes):
    """A script whose last value depends on every line arriving once, in order."""
    lines = ["--- hwtest %d" % nbytes, "_s=1"]
    s, k, size = 1, 0, sum(len(l) + 2 for l in lines)
    while True:
        k += 1
        line = "_s=(_s*31+%d)%%65521 -- %s" % (k, "pad" * ((k % 7) + 1))
        if size + len(line) + 2 > nbytes:
            break
        lines.append(line)
        size += len(line) + 2
        s = (s * 31 + k) % 65521
    return "\r\n".join(lines), s


def _upload_case(c, R, name, nbytes, mode, end="^^e"):
    script, expect = make_script(nbytes)
    fresh(c)
    dt, lines, ready = c.upload(script, end=end, mode=mode)
    got, detail = None, ""
    try:
        got = int(float(c.eval("_s", timeout=2.0)))
    except Exception as e:  # stuck in upload mode, or the script never ran
        detail = type(e).__name__
    ok = ready and got == expect
    said = [l for l in lines if l and not l.startswith("^^")]
    R.check(name + "_intact", ok, detail or ("" if ok else "sum %s != %s; said %r" % (got, expect, said[:2])))
    if ok:
        R.metric(name + "_time", dt * 1000, "ms", "lower")
        R.metric(name + "_rate", len(script) / dt / 1000, "kB/s", "higher")
    return ok


def t_upload(c, R):
    """Host to device: the same script sent politely, then with no pacing."""
    big = 15800  # just under the 16k limit
    _upload_case(c, R, "upload_druid_paced", big, "druid")
    _upload_case(c, R, "upload_lines_unpaced", big, "lines")
    _upload_case(c, R, "upload_burst", big, "burst")
    _upload_case(c, R, "upload_small_burst", 3000, "burst")

    # store one in flash, reboot, and read it back
    script, expect = make_script(big)
    fresh(c)
    dt, _, ready = c.upload(script, end="^^w", mode="druid")
    R.check("store_script", ready, "%.0f ms" % (dt * 1000))
    c.reset()
    said = [l for l in c.boot_lines if not l.startswith("^^")]  # what crow reports while loading it
    ran, got = False, None
    for _ in range(3):  # the first exchange after a reset can be lost to ModemManager
        try:
            got = int(float(c.eval("_s")))
            ran = got == expect
            break
        except Exception as e:
            got = type(e).__name__
            time.sleep(1.0)
            c.clear()
    R.check("stored_script_runs_at_boot", ran, "" if ran else "crow said %r" % said[:2])
    data, pt = c.print_script()
    body = data.replace(b"\n\r", b"").rstrip(b"\r\n\x00 ")
    same = body == script.encode()
    R.check("print_script_matches", same, "%d of %d bytes" % (len(body), len(script)))
    if same:
        R.metric("print_script_time", pt * 1000, "ms", "lower")
    lines = c.command("^^c", timeout=4.0, idle=1.5)
    R.check("clear_script", any("cleared" in l for l in lines), lines[:1])


def t_repl_flood(c, R):
    fresh(c)
    # 300 separate writes, back to back
    c.lua("_c=0")
    t0 = time.perf_counter()
    for _ in range(300):
        c.write("_c=_c+1\r\n")
    got = 0
    try:
        got = int(float(c.eval("_c", timeout=5.0)))
    except Exception:
        pass
    dt = time.perf_counter() - t0
    R.check("repl_300_writes_all_run", got == 300, "%d of 300" % got)
    R.metric("repl_lines_per_second", got / dt, "lines/s", "higher")
    # 100 lines in a single write. each is 37 bytes, so no 64-byte usb packet
    # ends on a line ending until well past the 2 kB line buffer
    fresh(c)
    c.lua("_c=0")
    line = "_c=_c+1 --" + "p" * 25 + "\r\n"
    assert len(line) == 37
    c.write(line * 100)
    time.sleep(0.5)
    got = 0
    try:
        got = int(float(c.eval("_c", timeout=5.0)))
    except Exception:
        pass
    R.check("repl_100_lines_one_write_all_run", got == 100, "%d of 100" % got)
    # one write is one chunk, as when norns sends a multi-line string. here the
    # first usb packet happens to end exactly on a line ending
    fresh(c)
    head = "function _f() --" + "p" * 46 + "\r\n"
    assert len(head) == 64
    c.write(head + "  return 7\r\nend\r\n")
    time.sleep(0.3)
    got = ""
    try:
        got = c.eval("_f and _f()", timeout=2.0)
    except Exception as e:
        got = type(e).__name__
    R.check("repl_multiline_write_is_one_chunk", got == "7", got)


def _time_to(c, code, needle, timeout=8.0):
    c.clear()
    t0 = time.perf_counter()
    c.write(code + "\r\n")
    lines, _ = c.read_lines(timeout, until=needle)
    dt = time.perf_counter() - t0
    c.read_lines(0.5, idle=0.2)  # the traceback that follows
    return dt if dt < timeout else None


def t_watchdog(c, R):
    fresh(c)
    for name, code in (("empty_loop", "while true do end"),
                       ("c_call_loop", "while true do get_state(1) end"),
                       ("print_loop", "while true do print('.') end"),
                       ("pcall_loop", "pcall(function() while true do end end)")):
        dt = _time_to(c, code, "CPU timed out.")
        alive = c.alive(2.0)
        R.check("watchdog_%s_recovers" % name, dt is not None and alive,
                "killed" if dt else "not killed in 8s")
        if dt is not None:
            R.metric("watchdog_%s" % name, dt * 1000, "ms")
        if not alive:
            fresh(c)
    # a runaway inside a clock coroutine
    c.clear()
    c.write("clock.run(function() clock.sleep(0.05) while true do end end)\r\n")
    lines, ok = c.read_lines(8.0, until="CPU timed out.")
    c.read_lines(0.5, idle=0.2)
    R.check("watchdog_clock_coroutine_recovers", ok and c.alive(2.0),
            "killed" if ok else "not killed in 8s")
    # a kill must not be something user code can catch and carry on from
    fresh(c)
    c.clear()
    c.write("_w=nil pcall(function() while true do end end) _w=1\r\n")
    c.read_lines(8.0, until="CPU timed out.")
    c.read_lines(0.5, idle=0.2)
    got = c.eval("_w") if c.alive(2.0) else "dead"
    R.check("watchdog_kill_not_swallowed_by_pcall", got == "nil", "_w=" + got)


def t_clock(c, R):
    fresh(c)
    c.telemetry_reset()
    # 50 coroutines waiting on the same beat all wake in the same millisecond
    c.lua("_k={} for i=1,50 do _k[i]=0 clock.run(function() while true do "
          "clock.sync(1/2) _k[i]=_k[i]+1 end end) end")
    time.sleep(2.6)
    out = c.lua("do local m,M=1e9,0 for i=1,50 do m=math.min(m,_k[i]) M=math.max(M,_k[i]) end "
                "print('#mm '..m..' '..M) end")
    lo, hi = (int(float(x)) for x in [l for l in out if l.startswith("#mm ")][-1].split()[1:])
    t = c.telemetry()
    c.lua("clock.cleanup()")
    R.check("clock_50_wake_together", lo >= 3 and hi - lo <= 1,
            "each ran %d to %d times, %d events dropped" % (lo, hi, t["event_full"]))
    R.metric("clock_burst_event_queue_peak", int(t["event_max"]), "of 40", "lower")


def t_io(c, R):
    """Outputs 1 and 2 are patched to inputs 1 and 2: do they still agree?"""
    fresh(c)
    worst = 0.0
    for ch in (1, 2):
        for v in (-4.0, 0.0, 3.0, 8.0):
            c.lua("output[%d].volts = %g" % (ch, v))
            time.sleep(0.08)
            got = c.num("input[%d].volts" % ch)
            worst = max(worst, abs(got - v))
    R.metric("loopback_worst_error", worst * 1000, "mV", "lower")
    R.check("loopback_within_150mV", worst < 0.15, "%.0f mV" % (worst * 1000))
    # a slew should read as a smooth ramp, not steps or glitches
    c.lua("output[1].volts = -3")
    time.sleep(0.2)
    c.lua("_r={} input[1].mode('stream', 0.004) input[1].stream = function(v) _r[#_r+1]=v end")
    c.lua("output[1].slew = 0.4 output[1].volts = 3")
    time.sleep(0.7)
    c.lua("input[1].mode('none')")
    c.lua("_j=0 for i=2,#_r do _j=math.max(_j, math.abs(_r[i]-_r[i-1])) end")
    jump = c.num("_j")
    n = c.num("#_r")
    R.metric("ramp_biggest_step", jump * 1000, "mV", "lower")
    R.check("ramp_is_smooth", n > 100 and jump < 0.25, "%d samples, %.0f mV" % (n, jump * 1000))


def t_memory(c, R):
    """How much can a script allocate before Lua refuses, and does crow survive?"""
    fresh(c)
    kb, alive = None, False
    try:
        out = c.lua("_t={} local ok=pcall(function() for i=1,400 do "
                    "_t[i]=string.rep('x',1024)..i end end) local n=#_t _t=nil "
                    "collectgarbage() print('#kb '..n)", timeout=6.0)
        kb = int([l for l in out if l.startswith("#kb ")][-1][4:])
        alive = c.alive(2.0)
    except Exception:
        pass
    R.check("out_of_memory_survived", alive and kb is not None)
    if kb is not None:
        R.metric("lua_allocatable", kb, "kB", "higher")
    if not alive:
        fresh(c)


FAULTS = ((0, "bus_fault"), (1, "undefined_instruction"), (2, "lua_panic"), (3, "stack_overflow"))


def t_fault(c, R):
    """Crash on purpose. A good firmware comes back by itself and says why."""
    for kind, name in FAULTS:
        fresh(c)
        c.clear()
        t0 = time.perf_counter()
        c.write("sys_telemetry('fault', %d)\r\n" % kind)
        time.sleep(0.3)
        c.close()
        back, report, kind_seen = False, "", 0
        try:
            c.wait_port(timeout=8.0, gone_first=True)
            c.open(settle=True)
            lines, _ = c.read_lines(4.0, idle=2.5)  # the report is sent ~1s after connecting
            report = next((l for l in lines if l.startswith("!crash")), "")
            back = c.alive(2.0) and c.num("time()") < 20000
            if back:
                kind_seen = int(c.telemetry().get("crash_kind", 0))
        except Exception:
            pass
        R.check("fault_%s_recovers" % name, back, "" if back else "no answer until reset by hand")
        if back:
            R.check("fault_%s_reported" % name, bool(report) and kind_seen != 0,
                    report[:90] or "no '!crash' line (crash_kind=%d)" % kind_seen)


def t_crashloop(c, R):
    """A stored script that crashes in init() must not lock the user out."""
    fresh(c)
    script = "--- hwtest crashloop\r\nfunction init() sys_telemetry('fault', 0) end"
    c.write("^^s"); time.sleep(0.2)
    c.write(script + "\r\n"); time.sleep(0.1)
    c.write("^^w")  # stores it, then runs init(): the first crash
    time.sleep(1.0)
    c.close()
    ok, said, boots = False, [], 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < 25.0:  # ride out the reboots until one stays up
        try:
            c.wait_port(timeout=5.0)
            c.open(settle=True)
            lines, _ = c.read_lines(4.0, idle=2.5)
            said += [l for l in lines if l.startswith("!crash")]
            if c.alive(1.0) and c.num("time()") > 3000:
                ok = True
                break
        except Exception:
            pass
        c.close()
        boots += 1
        time.sleep(0.2)
    skipped = any("skipped" in l for l in said)
    R.check("crash_loop_ends_with_working_repl", ok, "" if ok else "still rebooting after 25s")
    if ok:
        R.check("crash_loop_script_skipped_and_said_so", skipped, (said or ["nothing said"])[-1][:80])
        kept = b"hwtest crashloop" in c.print_script()[0]
        R.check("crash_loop_script_still_stored", kept)
        c.command("^^c", timeout=4.0, idle=1.5)


SECTIONS = [("build", t_build), ("boot", t_boot), ("ram", t_ram), ("cpu", t_cpu),
            ("lua", t_lua), ("latency", t_latency), ("tx", t_tx), ("upload", t_upload),
            ("repl", t_repl_flood), ("io", t_io), ("clock", t_clock),
            ("watchdog", t_watchdog), ("memory", t_memory), ("fault", t_fault), ("crashloop", t_crashloop)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--flash", help="firmware .bin to write first")
    ap.add_argument("--only", help="comma-separated sections: " + ",".join(n for n, _ in SECTIONS))
    ap.add_argument("--skip", default="")
    args = ap.parse_args()

    only = args.only.split(",") if args.only else None
    skip = args.skip.split(",")
    R = Results(args.label)
    c = Crow()
    if args.flash:
        print("flashing", args.flash)
        c.flash(args.flash)
        R.data["flashed"] = args.flash
    else:
        try:
            c.open()
        except Exception:  # left hung by something earlier
            c.reset()
    # start from no user script
    c.command("^^c", timeout=4.0, idle=1.5)

    for name, fn in SECTIONS:
        if (only and name not in only) or name in skip:
            continue
        print("[%s]" % name)
        try:
            fn(c, R)
        except Exception as e:
            R.check(name + "_section_completed", False, "%s: %s" % (type(e).__name__, e))
            try:
                c.reset()
            except Exception as e2:
                print("    could not recover:", e2)
                break

    try:
        c.reset()
        c.command("^^c", timeout=4.0, idle=1.5)
    except Exception:
        pass
    c.close()

    out = os.path.join(HERE, "results", args.label + ".json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if only and os.path.exists(out):  # a partial run updates the existing record
        old = json.load(open(out))
        old["metrics"].update(R.data["metrics"])
        old["checks"].update(R.data["checks"])
        old["date"] = R.data["date"]
        R.data = old
    json.dump(R.data, open(out, "w"), indent=1)
    fails = [k for k, v in R.data["checks"].items() if not v["ok"]]
    print("\n%d metrics, %d checks, %d failing%s" % (
        len(R.data["metrics"]), len(R.data["checks"]), len(fails),
        (": " + ", ".join(fails)) if fails else ""))
    print("wrote", out)


if __name__ == "__main__":
    main()
