# hardware test suite

Measures a real crow and checks that it behaves. Run it before and after a
firmware change to see whether the change did what it was meant to.

```
python3 tests/hw/run.py --label baseline              # test what is on the module
python3 tests/hw/run.py --label mychange --flash crow.bin
python3 tests/hw/run.py --label x --only cpu,lua      # some sections only
python3 tests/hw/report.py baseline mychange          # markdown comparison
```

## bench

- crow on USB (serial), found by its `/dev/serial/by-id` name
- an ST-Link on crow's SWD header (`st-flash`), used to flash, to reset between
  sections, and to recover a module the tests have crashed on purpose
- output 1 patched to input 1, output 2 to input 2
- python3 with pyserial, and the arm toolchain for the size figures

The suite overwrites the stored user script and leaves the module with none.

On Linux, ModemManager opens every new serial port for a few seconds and types
into it. The suite waits 6 s after each reset when ModemManager is running,
unless this rule tells it to leave crow alone (several minutes faster):

```
# /etc/udev/rules.d/71-crow.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", ENV{ID_MM_DEVICE_IGNORE}="1"
```

## what it reads from the firmware

`sys_telemetry()` is a Lua function compiled in by default (`make TELEMETRY=0`
removes it and every hook behind it; see `lib/telemetry.c`).

| call | returns |
| --- | --- |
| `sys_telemetry()` | a table of everything measured since the last reset of the window |
| `sys_telemetry('reset')` | starts a new window |
| `sys_telemetry('us')` | microseconds from the CPU cycle counter (wraps every ~19 s) |
| `sys_telemetry('fault', n)` | crashes on purpose: 0 bus fault, 1 undefined instruction, 2 Lua panic, 3 stack overflow |

Fields: `cpu_busy` (share of time not spent in an uninterrupted idle pass of the
main loop), `loop_min_us` / `loop_max_us` (shortest and longest pass),
`dsp_avg_us` / `dsp_max_us` / `dsp_load` (the audio block interrupt),
`ram_static`, `heap_arena`, `heap_used`, `heap_free`, `stack_peak`, `ram_gap`
(RAM the heap has not yet claimed), `lua_bytes`, `rx_bytes`,
`tx_bytes`, `tx_dropped`, `rx_overflow`, `event_full`, `event_max`,
`reset_cause`, `uptime_ms`.

## sections

| section | measures | checks |
| --- | --- | --- |
| build | image size, static RAM | |
| boot | reset to USB enumeration | `^^v`, `^^i` answer |
| ram | Lua heap at boot, RAM left for scripts, stack peak | |
| cpu | idle and busy-patch CPU share, audio block time, worst loop stall | nothing dropped under load |
| lua | five small Lua benchmarks, timed on the device | |
| latency | REPL round trip | |
| tx | printing 20 and 300 lines flat out | every line arrives intact |
| upload | a 15.8 kB script sent druid-style, per line unpaced, and as one write | each arrives intact; store, reboot, `^^p` |
| repl | 300 lines as 300 writes, and as one write | all run |
| io | output to input loopback error, ramp smoothness | within tolerance |
| clock | 60 clocks waking in the same millisecond | all resume |
| watchdog | time to kill runaway loops | killed, and crow answers afterwards |
| memory | how much a script can allocate | survives running out |
| fault | | comes back by itself after each crash, and says why |

Results are JSON in `results/`, one file per label.
