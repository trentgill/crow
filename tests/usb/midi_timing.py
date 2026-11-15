#!/usr/bin/env python3
"""MIDI -> jack timing on crow, out2 -> in2 patched.

    python3 tests/usb/midi_timing.py --serial 206135773756

1. jitter: puts crow in audio mode (PC 127 on channel 16), records the inputs
   through USB audio, and plays a note on channel 1 every 25 ms. The trigger
   envelope on out2 comes back on in2; the spread of the edge-to-edge
   intervals (sample accurate) is the jitter of host -> crow -> jack.
2. cost: how many voice messages a second crow's Lua behaviours keep up with
   (a burst; the USB ring holds the host off, so nothing is lost).
Leaves crow in audio mode off again (PC 126).
"""
import argparse, glob, os, re, subprocess, sys, tempfile, time, wave
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from check import find_dev, alsa_card, serial_port
import serial

FS = 48000

def rawmidi(card):
    return os.open("/dev/snd/midiC%sD0" % card, os.O_RDWR)

def wait_card(sn, audio, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        dev = find_dev(sn)
        if dev is not None:
            card = alsa_card(dev)
            if card and os.path.exists("/proc/asound/card%s/stream0" % card) == audio:
                time.sleep(1.0)
                return dev, card
        time.sleep(0.2)
    sys.exit("crow did not come back %s audio" % ("with" if audio else "without"))

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--serial", required=True)
    ap.add_argument("--notes", type=int, default=200)
    ap.add_argument("--interval", type=float, default=0.025)
    a = ap.parse_args()
    dev, card = wait_card(a.serial, False, 2) if not os.path.exists("/proc/asound/card%s/stream0" % alsa_card(find_dev(a.serial))) else (find_dev(a.serial), alsa_card(find_dev(a.serial)))
    fd = rawmidi(card)
    os.write(fd, bytes([0xCF, 120]))  # default map
    if not os.path.exists("/proc/asound/card%s/stream0" % card):
        os.write(fd, bytes([0xCF, 127])); os.close(fd)
        dev, card = wait_card(a.serial, True)
        fd = rawmidi(card)
    os.write(fd, bytes([0xB0, 75, 30]))  # ~10 ms decay: one clean edge per note

    d = tempfile.mkdtemp(); rec = os.path.join(d, "rec.wav")
    secs = a.notes * a.interval + 1.5
    r = subprocess.Popen(["arecord", "-q", "-D", "hw:%s,0" % card, "-c", "2", "-f", "S16_LE",
                          "-r", str(FS), "-d", str(int(secs + 0.5)), rec])
    time.sleep(0.7)
    t = time.perf_counter()
    sent = []
    for i in range(a.notes):
        t += a.interval
        while time.perf_counter() < t: pass  # busy wait: host timer jitter << 0.1 ms
        os.write(fd, bytes([0x90, 60, 127]))
        sent.append(time.perf_counter())
        os.write(fd, bytes([0x80, 60, 0]))
    r.wait()
    w = wave.open(rec); x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2) / 3276.8
    y = x[:, 1]
    up = np.nonzero((y[1:] > 2.0) & (y[:-1] <= 2.0))[0]
    # keep one edge per note
    edges = [up[0]] if len(up) else []
    for e in up[1:]:
        if e - edges[-1] > FS * a.interval / 2: edges.append(e)
    edges = np.array(edges)
    sent = np.array(sent)
    host = np.diff(sent) * 1000
    print("host send intervals: %.3f ms mean, %.3f ms std" % (host.mean(), host.std()))
    if len(edges) < a.notes * 0.9:
        print("only %d of %d notes seen on in2" % (len(edges), a.notes))
    iv = np.diff(edges) / FS * 1000
    print("%d notes, jack edge intervals: mean %.3f ms, std %.3f ms, min %.3f, max %.3f (p-p %.3f ms)" %
          (len(edges), iv.mean(), iv.std(), iv.min(), iv.max(), iv.max() - iv.min()))

    # cost: a burst of note on/off pairs, timed by crow itself
    s = serial.Serial(serial_port(a.serial), 115200, timeout=0.3)
    s.write(b"\x1b"); time.sleep(0.1); s.reset_input_buffer()
    s.write(b"_n = 0 _t0 = nil midi.receive = function() _n = _n + 1 if not _t0 then _t0 = time() end _t1 = time() end\n")
    time.sleep(0.2)
    burst = bytes([0x90, 60, 100, 0x80, 60, 0] * 1000)
    os.write(fd, burst); time.sleep(3)
    s.reset_input_buffer(); s.write(b"print(_n, _t1 - _t0) midi.receive = nil\n"); time.sleep(0.3)
    out = s.read(s.in_waiting or 1).decode().split()
    n, ms = int(out[0]), float(out[1])
    print("burst: %d voice messages in %.0f ms on crow = %.0f per second (%.0f us each)" % (n, ms, n / ms * 1000, ms * 1000 / n))
    os.write(fd, bytes([0xCF, 126])); os.close(fd)
    return 0

if __name__ == "__main__":
    sys.exit(main())
