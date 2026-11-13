"""Talk to a real crow over USB serial, and reset/flash it over ST-Link.

Everything the hardware suite needs to know about the wire lives here, so the
tests read as what they measure. Commands are framed the way druid frames them
(a `^^x` command alone in its own write, lines ending in CRLF), which is the
behaviour every firmware version has to keep accepting.
"""

import glob
import os
import subprocess
import time

import serial

PORT_GLOB = "/dev/serial/by-id/usb-monome*crow*"
APP_ADDRESS = "0x08020000"
US_WRAP = (1 << 32) // 216  # sys_telemetry('us') wraps with the 32-bit cycle counter


def find_port(usb_serial=None):
    """crow's serial port. With several crows attached, name one by its USB
    serial number (or set CROW_SERIAL) rather than taking the first."""
    usb_serial = usb_serial or os.environ.get("CROW_SERIAL")
    ports = sorted(glob.glob(PORT_GLOB))
    if usb_serial:
        ports = [p for p in ports if p.endswith("_%s-if00" % usb_serial)]
    return ports[0] if ports else None


STLINK_PRODUCTS = ("3744", "3748", "374b", "374e", "374f", "3752", "3753", "3754")


def attached_stlinks():
    """Serial numbers of the ST-Links on this machine, read from sysfs so that
    no probe (and no target behind one) is touched."""
    found = []
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if open(dev + "/idVendor").read().strip() != "0483":
                continue
            if open(dev + "/idProduct").read().strip() not in STLINK_PRODUCTS:
                continue
            found.append(open(dev + "/serial").read().strip())
        except OSError:
            pass
    return found


def find_stlink():
    """Serial number of the ST-Link wired to crow.

    Never let st-flash choose: with a second probe on the machine it takes
    whichever it finds first and will happily erase the wrong board. Set
    CROW_STLINK to the serial of crow's probe. Left unset, the only attached
    ST-Link is used, and with more than one the suite refuses to guess.
    """
    serial = os.environ.get("CROW_STLINK")
    if serial:
        return serial
    probes = attached_stlinks()
    if len(probes) != 1:
        raise RuntimeError("%d ST-Links attached (%s): set CROW_STLINK to the serial of the "
                           "one wired to crow" % (len(probes), ", ".join(probes) or "none"))
    return probes[0]


def modemmanager_active():
    r = subprocess.run(["systemctl", "is-active", "ModemManager"],
                       capture_output=True, text=True)
    return r.stdout.strip() == "active"


def modemmanager_ignores(port):
    """True when a udev rule has told ModemManager to leave this port alone."""
    r = subprocess.run(["udevadm", "info", "-q", "property", "-n", port],
                       capture_output=True, text=True)
    return "ID_MM_DEVICE_IGNORE=1" in r.stdout


class Crow:
    def __init__(self, settle=None, usb_serial=None, stlink=None):
        self.usb_serial = usb_serial
        self.ser = None
        self.events = []  # ^^event(...) lines seen while waiting for something else
        self.boot_lines = []  # what crow said while starting, after the last reset
        self._stlink = stlink
        self._n = 0
        # ModemManager probes a new tty for a few seconds and types 'test' into it,
        # unless a udev rule tells it not to (see README.md)
        self.settle = settle
        self.mm = modemmanager_active()

    # ---- connection

    def wait_port(self, timeout=15.0, gone_first=False):
        t0 = time.monotonic()
        if gone_first:
            while find_port(self.usb_serial) and time.monotonic() - t0 < 3.0:
                time.sleep(0.005)
        while time.monotonic() - t0 < timeout:
            p = find_port(self.usb_serial)
            if p:
                return p, time.monotonic() - t0
            time.sleep(0.005)
        raise TimeoutError("crow did not enumerate")

    def open(self, settle=True):
        port, _ = self.wait_port()
        if settle:
            wait = self.settle
            if wait is None:
                wait = 6.0 if (self.mm and not modemmanager_ignores(port)) else 0.3
            time.sleep(wait)
        for _ in range(50):  # the node can exist before it is openable
            try:
                self.ser = serial.Serial(port, 115200, timeout=0.02, write_timeout=3.0)
                break
            except (serial.SerialException, OSError):
                time.sleep(0.1)
        else:
            raise TimeoutError("could not open " + port)
        self.clear()
        return self

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def clear(self):
        """ESC empties crow's line buffer; then drop whatever is in flight to us."""
        self.ser.write(b"\x1b")
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        self._rx = b""

    # ---- st-link

    def stflash(self, *args, timeout=30):
        if self._stlink is None:
            self._stlink = find_stlink()
        return subprocess.run(["st-flash", "--serial", self._stlink, *args],
                              capture_output=True, text=True, timeout=timeout)

    def reset(self, settle=True):
        """Hardware reset. Returns seconds until the USB port reappeared."""
        self.close()
        r = self.stflash("reset")
        if r.returncode:
            raise RuntimeError("st-flash reset failed:\n" + r.stderr[-400:])
        _, dt = self.wait_port(gone_first=True)
        self.open(settle=settle)
        self._boot_text()
        return dt

    def _boot_text(self):
        """crow holds its boot messages for 500ms after the host connects, and
        ends them with ^^ready(). Collect them so they don't land in a test."""
        lines, _ = self.read_lines(1.5, until="^^ready()")
        self.boot_lines = [l for l in lines if l]

    def flash(self, binfile):
        self.close()
        r = self.stflash("--reset", "write", binfile, APP_ADDRESS, timeout=120)
        if "jolly good" not in r.stderr + r.stdout:
            raise RuntimeError("st-flash failed:\n" + r.stderr[-800:])
        self.wait_port(gone_first=True)
        self.open()
        self._boot_text()

    def alive(self, timeout=1.0):
        try:
            self.clear()
            return "1" in self.lua("print(1)", timeout=timeout)
        except Exception:
            return False

    # ---- raw io

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.ser.write(data)

    def read_lines(self, timeout, until=None, idle=None):
        """Collect lines until one equals `until`, the port stays quiet for
        `idle` seconds, or `timeout` passes. crow ends lines with LF CR."""
        lines = []
        t0 = last = time.monotonic()
        while True:
            chunk = self.ser.read(self.ser.in_waiting or 1)  # return as soon as anything is here
            now = time.monotonic()
            if chunk:
                last = now
                self._rx += chunk
                while b"\n" in self._rx:
                    raw, self._rx = self._rx.split(b"\n", 1)
                    line = raw.strip(b"\r").decode(errors="replace")
                    if until is not None and line == until:
                        return lines, True
                    lines.append(line)
            if now - t0 > timeout:
                return lines, False
            if idle is not None and now - last > idle:
                return lines, False

    # ---- lua

    def lua(self, code, timeout=3.0, gap=0.004):
        """Run one chunk and return the lines it printed (events filtered out).
        A sentinel print in a second write marks the end, so this works whether
        or not the firmware evaluates the two writes separately."""
        self._n += 1
        mark = "#ok%d" % self._n
        self.write(code + "\r\n")
        time.sleep(gap)
        self.write("print('%s')\r\n" % mark)
        lines, ok = self.read_lines(timeout, until=mark)
        if not ok:
            raise TimeoutError("no reply to %r (got %r)" % (code[:60], lines[-5:]))
        out = []
        for l in lines:
            (self.events if l.startswith("^^") else out).append(l)
        return out

    def eval(self, expr, timeout=3.0):
        out = self.lua("print('='..tostring(%s))" % expr, timeout)
        vals = [l[1:] for l in out if l.startswith("=")]
        if not vals:
            raise RuntimeError("eval %r printed %r" % (expr, out))
        return vals[-1]

    def num(self, expr, timeout=3.0):
        return float(self.eval(expr, timeout))

    def command(self, cmd, timeout=1.0, idle=0.25):
        """Send a ^^ command alone, as hosts do, and return what came back."""
        self.write(cmd)
        lines, _ = self.read_lines(timeout, idle=idle)
        return lines

    def telemetry(self):
        # two prints, so neither comes near the size of the device's tx buffer
        code = ("do local t=sys_telemetry() local s={} for k,v in pairs(t) do "
                "s[#s+1]=k..'='..tostring(v) if #s==14 then print('#tm '..table.concat(s,' ')) s={} end "
                "end print('#tm '..table.concat(s,' ')) end")
        for attempt in range(4):
            out = self.lua(code)
            got = [l for l in out if l.startswith("#tm ")]
            try:
                t = {k: float(v) for l in got for k, v in (kv.split("=") for kv in l[4:].split())}
            except ValueError:
                t = {}
            if "uptime_ms" in t and "cpu_busy" in t and "lua_bytes" in t:
                return t
            time.sleep(0.3)  # a reply cut short: the tx buffer was still full
        raise RuntimeError("no telemetry: %r" % out[:3])

    def telemetry_reset(self):
        self.lua("sys_telemetry('reset')")

    def timed(self, body, timeout=5.0):
        """Microseconds the device took to run `body`, by its own cycle counter."""
        out = self.lua("do local u=sys_telemetry local t0=u('us') %s "
                       "print('#us '..(u('us')-t0)) end" % body, timeout)
        got = [l for l in out if l.startswith("#us ")]
        if not got:
            raise RuntimeError("timed chunk did not finish: %r" % out[:3])
        return int(float(got[-1][4:])) % US_WRAP

    # ---- scripts

    def upload(self, script, end="^^e", mode="druid", timeout=20.0):
        """Send a script. Returns (seconds until crow answered, lines received).

        mode 'druid'  : ^^s, 200ms, one write per line with 1ms between, 100ms, end
        mode 'lines'  : one write per line, no waiting at all
        mode 'burst'  : the whole body in a single write, no waiting at all
        """
        body = script.replace("\r\n", "\n").split("\n")
        t0 = time.monotonic()
        self.write("^^s")
        if mode == "druid":
            time.sleep(0.2)
            for line in body:
                self.write(line + "\r\n")
                time.sleep(0.001)
            time.sleep(0.1)
        elif mode == "lines":
            for line in body:
                self.write(line + "\r\n")
        elif mode == "burst":
            self.write("".join(line + "\r\n" for line in body))
        else:
            raise ValueError(mode)
        self.write(end)
        self.ser.flush()
        # '^^ready()' is the last thing crow says once the script has run
        lines, ok = self.read_lines(timeout, until="^^ready()")
        return time.monotonic() - t0, lines, ok

    def print_script(self, timeout=10.0):
        """^^p: the stored script, byte for byte as crow sends it."""
        self.clear()
        self.write("^^p")
        data = b""
        t0 = last = time.monotonic()
        while time.monotonic() - t0 < timeout:
            chunk = self.ser.read(4096)
            if chunk:
                data += chunk
                last = time.monotonic()
            elif data and time.monotonic() - last > 0.4:
                break
        return data, last - t0
