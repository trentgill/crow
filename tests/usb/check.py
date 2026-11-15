#!/usr/bin/env python3
"""Host-side checks for crow's composite USB device (serial + MIDI [+ audio]).

    python3 tests/usb/check.py --serial 206635743756 [--audio]

1. descriptors: device class, interfaces & endpoints the firmware promises
2. serial: ^^v answers on the CDC port (the port druid & norns use)
3. MIDI: a Lua echo (midi.receive -> midi.send) round-trips through ALSA
Run as the desktop user (ALSA rawmidi needs no root). Exits non-zero on failure.
"""
import argparse, glob, os, re, subprocess, sys, time
import usb.core
import serial

VID, PID = 0x0483, 0x5740
ok = True

def check(cond, what):
    global ok
    print(("  ok   " if cond else "  FAIL ") + what)
    ok = ok and bool(cond)

def find_dev(sn):
    for d in usb.core.find(idVendor=VID, idProduct=PID, find_all=True):
        try:
            if d.serial_number == sn: return d
        except (usb.core.USBError, ValueError): pass
    return None

def descriptors(dev, audio):
    print("descriptors")
    check((dev.bDeviceClass, dev.bDeviceSubClass, dev.bDeviceProtocol) == (0xEF, 2, 1), "device class EF/02/01 (IAD)")
    check(dev.bcdDevice == (0x0301 if audio else 0x0300), "bcdDevice %04x" % dev.bcdDevice)
    cfg = dev.get_active_configuration()
    itfs = {(i.bInterfaceNumber, i.bAlternateSetting): i for i in cfg}
    check(cfg.bNumInterfaces == (7 if audio else 4), "%d interfaces" % cfg.bNumInterfaces)
    want = {(0, 0): (2, 2, {0x82}), (1, 0): (0x0A, 0, {0x01, 0x81}),
            (2, 0): (1, 1, set()), (3, 0): (1, 3, {0x02, 0x83})}
    if audio:
        want.update({(4, 0): (1, 1, set()), (5, 0): (1, 2, set()), (5, 1): (1, 2, {0x03, 0x85}),
                     (6, 0): (1, 2, set()), (6, 1): (1, 2, {0x84})})
    for k, (cls, sub, eps) in want.items():
        i = itfs.get(k)
        got = None if i is None else (i.bInterfaceClass, i.bInterfaceSubClass, {e.bEndpointAddress for e in i})
        check(got == (cls, sub, eps), "interface %d alt %d: class %s eps %s" % (k[0], k[1], got and got[:2], got and sorted(map(hex, got[2]))))

def serial_port(sn):
    p = glob.glob("/dev/serial/by-id/*%s-if00" % sn)
    return p[0] if p else None

def repl(s, line, wait=0.3):
    s.reset_input_buffer()
    s.write(line.encode() + b"\n")
    time.sleep(wait)
    return s.read(s.in_waiting or 1).decode(errors="replace")

def serial_check(sn):
    print("serial")
    port = serial_port(sn)
    check(port, "CDC port present on interface 0 (%s)" % port)
    if not port: return None
    s = serial.Serial(port, 115200, timeout=0.5)
    s.write(b"\x1b"); time.sleep(0.1)
    v = repl(s, "^^v")
    check("^^version" in v, "^^v answers: %r" % v.strip()[:60])
    return s

def alsa_card(dev):
    """ALSA card number of this USB device (several crows can be attached)."""
    want = "%03d/%03d" % (dev.bus, dev.address)
    for p in glob.glob("/proc/asound/card*/usbbus"):
        if open(p).read().strip() == want:
            return re.search(r"card(\d+)", p).group(1)
    return None

def midi_port(dev):
    card = alsa_card(dev)
    out = subprocess.run(["amidi", "-l"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) > 1 and f[1].startswith("hw:%s," % card):
            return f[1]
    return None

def midi_check(s, dev):
    print("MIDI")
    port = midi_port(dev)
    check(port, "ALSA rawmidi port %s" % port)
    if not port or not s: return
    repl(s, "midi.receive = function(...) midi.send(...) end")
    msgs = ["90 3C 64", "80 3C 00", "B0 07 7F", "E0 00 40", "C0 05", "F8", "FA", "FC"]
    p = subprocess.Popen(["amidi", "-p", port, "-a", "-c", "-d", "-t", "1.0"], stdout=subprocess.PIPE, text=True)
    time.sleep(0.3)
    subprocess.run(["amidi", "-p", port, "-S", " ".join(msgs)], check=True)
    got = p.communicate()[0].split()
    want = " ".join(msgs).split()
    check(got == want, "echo of %d messages (%d bytes back)" % (len(msgs), len(got)))
    # amidi hides clock (F8) & active sensing unless -c & -a
    # a burst bigger than one USB packet and the rings
    burst = " ".join("90 %02X 40 80 %02X 00" % (n, n) for n in range(128))
    p = subprocess.Popen(["amidi", "-p", port, "-a", "-c", "-d", "-t", "1.5"], stdout=subprocess.PIPE, text=True)
    time.sleep(0.3)
    subprocess.run(["amidi", "-p", port, "-S", burst], check=True)
    got = p.communicate()[0].split()
    check(got == burst.split(), "burst of 256 notes echoed in order (%d of %d bytes)" % (len(got), len(burst.split())))
    repl(s, "midi.receive = nil")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True, help="crow's USB serial number")
    ap.add_argument("--audio", action="store_true", help="expect audio mode")
    a = ap.parse_args()
    dev = find_dev(a.serial)
    if dev is None: sys.exit("no crow with serial %s" % a.serial)
    descriptors(dev, a.audio)
    s = serial_check(a.serial)
    midi_check(s, dev)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
