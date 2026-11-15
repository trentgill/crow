#!/usr/bin/env python3
"""MIDI behaviours on a real crow, out1 -> in1 and out2 -> in2 patched.

    python3 tests/usb/midi_behaviours.py --serial 206135773756

Sends MIDI from ALSA (amidi) on the default channel map and reads the
voltages back through crow's own inputs over the serial port.
"""
import argparse, os, subprocess, sys, time
import usb.core, serial
sys.path.insert(0, __import__("os").path.dirname(__file__))
from check import find_dev, midi_port, serial_port

ok = True
def check(cond, what):
    global ok
    print(("  ok   " if cond else "  FAIL ") + what)
    ok = ok and bool(cond)

class Crow:
    def __init__(self, sn):
        self.s = serial.Serial(serial_port(sn), 115200, timeout=0.2)
        self.s.write(b"\x1b"); time.sleep(0.1); self.s.reset_input_buffer()
    def lua(self, line, wait=0.15):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\n"); time.sleep(wait)
        return self.s.read(self.s.in_waiting or 1).decode(errors="replace").strip()
    def ins(self):
        r = self.lua("print(input[1].volts, input[2].volts)")
        try:
            a, b = r.split()[:2]; return float(a), float(b)
        except ValueError:
            return None, None

_fd = {}
def send(port, *msg):
    """straight to the rawmidi device: amidi -S can sit on a message ~30 ms"""
    if port not in _fd:
        card = port.split(":")[1].split(",")[0]
        _fd[port] = os.open("/dev/snd/midiC%sD0" % card, os.O_WRONLY)
    os.write(_fd[port], bytes(msg))

def near(v, want, tol=0.05):
    return v is not None and abs(v - want) <= tol

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--serial", required=True)
    a = ap.parse_args()
    dev = find_dev(a.serial)
    port = midi_port(dev)
    c = Crow(a.serial)
    c.lua("midi.reset()")

    print("ch1: trigger voice on out1 (v8) + out2 (env)")
    for note, v in ((60, 0.0), (72, 1.0), (48, -1.0), (67, 7 / 12)):
        send(port, 0x90, note, 127); time.sleep(0.05)
        v1, _ = c.ins()
        check(near(v1, v, 0.02), "note %d -> %.3f V (want %.3f)" % (note, v1 or 0, v))
        send(port, 0x80, note, 0)
    send(port, 0xB0, 75, 127)  # decay 10 s, so REPL reads can follow it
    send(port, 0x90, 60, 127); time.sleep(0.02) # serial can overtake MIDI on the bus
    _, e0 = c.ins()
    time.sleep(1.0); _, e1 = c.ins()
    check(e0 is not None and e0 > 4.0 and 0.1 < e1 < e0 * 0.8,
          "env decays: %.2f V just after, %.2f V a second later" % (e0 or 0, e1 or 0))
    send(port, 0x80, 60, 0)
    send(port, 0xB0, 75, 64)
    send(port, 0xE0, 0x7F, 0x7F); time.sleep(0.05)  # bend full up
    send(port, 0x90, 60, 100); time.sleep(0.05)
    v1, _ = c.ins()
    check(near(v1, 2 / 12 * 8191 / 8192, 0.02), "pitch bend +2 st -> %.3f V" % (v1 or 0))
    send(port, 0xE0, 0x00, 0x40); send(port, 0x80, 60, 0)
    # last-note priority
    send(port, 0x90, 60, 100); send(port, 0x90, 64, 100); time.sleep(0.05)
    v_hi, _ = c.ins()
    send(port, 0x80, 64, 0); time.sleep(0.05)
    v_back, _ = c.ins()
    check(near(v_hi, 4 / 12, 0.02) and near(v_back, 0.0, 0.02), "last note wins, falls back: %.3f then %.3f V" % (v_hi or 0, v_back or 0))
    send(port, 0x80, 60, 0)

    print("ch3: ADSR voice on out1/out2 (takes the jacks from ch1)")
    send(port, 0xB2, 73, 0); send(port, 0xB2, 75, 0)  # fastest attack & decay
    send(port, 0x92, 62, 127); time.sleep(0.2)
    v1, held = c.ins()
    check(near(v1, 2 / 12, 0.02), "note 62 -> %.3f V" % (v1 or 0))
    check(near(held, 5.0, 0.15), "sustain while held: %.2f V (want 5)" % (held or 0))
    send(port, 0x82, 62, 0); time.sleep(0.1)
    send(port, 0xB2, 79, 64)  # sustain for the next note
    send(port, 0x92, 62, 127); time.sleep(0.2)
    _, held = c.ins()
    check(near(held, 64 * 8 / 127, 0.15), "CC 79 sets the next note's sustain: %.2f V (want %.2f)" % (held or 0, 64 * 8 / 127))
    send(port, 0x82, 62, 0); time.sleep(1.5)
    _, rel = c.ins()
    check(near(rel, 0.0, 0.05), "released: %.3f V" % (rel or 0))

    print("ch4: CC 16/17 -> out1/out2")
    for val in (0, 64, 127):
        send(port, 0xB3, 16, val); send(port, 0xB3, 17, 127 - val); time.sleep(0.05)
        v1, v2 = c.ins()
        check(near(v1, val * 10 / 127, 0.05) and near(v2, (127 - val) * 10 / 127, 0.05),
              "CC16=%d CC17=%d -> %.3f / %.3f V" % (val, 127 - val, v1 or 0, v2 or 0))

    print("ch5: voice + velocity: out1 v8, out2 env")
    send(port, 0x94, 69, 64); time.sleep(0.05)
    v1, _ = c.ins()
    check(near(v1, 9 / 12, 0.02), "note 69 -> %.3f V" % (v1 or 0))
    send(port, 0x84, 69, 0)

    print("ch6: duophonic, round robin over out1/2 and out3/4")
    send(port, 0x95, 72, 100); time.sleep(0.05)
    v_a, _ = c.ins()
    send(port, 0x95, 48, 100); time.sleep(0.05)
    v_b, _ = c.ins()
    check(near(v_a, 1.0, 0.02) and near(v_b, 1.0, 0.02),
          "1st note on out1 (%.3f V), 2nd goes to out3, out1 holds (%.3f V)" % (v_a or 0, v_b or 0))
    send(port, 0x85, 72, 0); send(port, 0x85, 48, 0); time.sleep(0.05)
    send(port, 0x95, 60, 100); time.sleep(0.05)
    v_c, _ = c.ins()
    check(near(v_c, 0.0, 0.02), "with both free, the next note is back on out1: %.3f V" % (v_c or 0))
    send(port, 0x85, 60, 0)

    print("ch10: drum triggers (kick -> out1)")
    c.lua("_k = 0 input[1].mode('change', 2, 0.5, 'rising') input[1].change = function() _k = _k + 1 end")
    for _ in range(5):
        send(port, 0x99, 36, 127); time.sleep(0.05)
    send(port, 0x99, 38, 127); time.sleep(0.05) # snare is out2: not counted
    r = c.lua("print(_k)")
    check(r.strip() == "5", "5 kicks -> %s rising edges on in1" % r.strip())
    c.lua("input[1].mode('none')")

    print("ch16 raw handler")
    r = c.lua("midi.ch[16] = function(s,a,b) print('raw',s,a,b) end")
    c.s.reset_input_buffer(); send(port, 0x9F, 1, 2); time.sleep(0.1)
    r = c.s.read(c.s.in_waiting or 1).decode(errors="replace")
    check("raw\t159\t1\t2" in r, "midi.ch[16] function gets the message: %r" % r.strip())
    c.lua("midi.reset()")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
