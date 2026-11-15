#!/usr/bin/env python3
"""Loopback test of crow's USB audio through real jacks (out1->in1, out2->in2).

    python3 tests/usb/audio_loop.py --card 4 [--seconds 10]

crow must already be in audio mode (usb.audio(true)). Plays on outputs 1 & 2
while recording inputs 1 & 2 through ALSA (hw:<card>,0), then reports:
- DC mapping: sample 0.25 / 0.5 / -0.25 should read back as +2.5 / +5 / -2.5 V
  (1.0 = 10 V both ways, so the loop gain is 1)
- a 1 kHz sine's level and THD+N
- the play-to-record offset of a step edge (includes the two programs' start gap)
- dropouts: discontinuities in a slow ramp
"""
import argparse, subprocess, sys, tempfile, time, wave, os
import numpy as np

FS = 48000

def tone_plan(seconds):
    t = np.arange(int(FS * seconds)) / FS
    out = np.zeros((len(t), 4))
    seg = int(FS * 0.5)
    # 0..0.5s silence, then DC steps, then a sine, then a ramp
    marks = {}
    i = seg
    for v in (0.25, 0.5, -0.25, 0.0):
        out[i:i + seg, 0] = v; out[i:i + seg, 1] = -v
        marks.setdefault("dc", []).append((i, v)); i += seg
    n = seg * 2
    out[i:i + n, 0] = 0.25 * np.sin(2 * np.pi * 1000 * t[:n])
    out[i:i + n, 1] = 0.25 * np.sin(2 * np.pi * 1000 * t[:n])
    marks["sine"] = (i, n); i += n
    n = len(t) - i - seg
    out[i:i + n, 0] = np.linspace(-0.2, 0.5, n)
    out[i:i + n, 1] = np.linspace(0.5, -0.2, n)
    marks["ramp"] = (i, n)
    return out, marks

def write_wav(path, x, ch):
    w = wave.open(path, "wb"); w.setnchannels(ch); w.setsampwidth(2); w.setframerate(FS)
    w.writeframes((np.clip(x, -1, 32767 / 32768) * 32768).astype("<i2").tobytes()); w.close()

def read_wav(path):
    w = wave.open(path)
    return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels()) / 32768.0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", required=True)
    ap.add_argument("--seconds", type=float, default=6.0)
    a = ap.parse_args()
    dev = "hw:%s,0" % a.card
    out, marks = tone_plan(a.seconds)
    d = tempfile.mkdtemp()
    play, rec = os.path.join(d, "play.wav"), os.path.join(d, "rec.wav")
    write_wav(play, out, 4)
    r = subprocess.Popen(["arecord", "-q", "-D", dev, "-c", "2", "-f", "S16_LE", "-r", str(FS),
                          "-d", str(int(a.seconds + 2)), rec])
    time.sleep(0.5)
    subprocess.run(["aplay", "-q", "-D", dev, play], check=True)
    r.wait()
    x = read_wav(rec) * 10.0  # volts
    played = out[:, :2] * 10.0

    # offset between the two programs' streams: cross-correlate input 1
    # against what was played on output 1
    n = len(x)
    p0 = np.zeros(n); m = min(n, len(played)); p0[:m] = played[:m, 0]
    xc = np.fft.irfft(np.fft.rfft(x[:, 0] - x[:, 0].mean(), 2 * n) *
                      np.conj(np.fft.rfft(p0 - p0.mean(), 2 * n)))
    lag = int(np.argmax(xc[:n]))
    if xc[lag] <= 0:
        sys.exit("nothing correlates on input 1: is out1 patched to in1?")
    print("recording lags playback by %d frames (%.1f ms, includes the start gap)" % (lag, lag / FS * 1000))
    edge = marks["dc"][0][0]

    def seg(i, n, settle=0.1):
        # skip the settling at the start, and stop short of the next segment
        s = int(i + lag + FS * settle); return x[s:int(i + lag + n - FS * 0.01)]
    ok = True
    print("DC (expect in1 = v*10, in2 = -v*10)")
    for i, v in marks["dc"]:
        y = seg(i, FS * 0.5)
        m = y.mean(0)
        err = max(abs(m[0] - v * 10), abs(m[1] + v * 10))
        good = err < 0.15
        ok &= good
        print("  %+.2f -> in1 %+.3f V  in2 %+.3f V  err %.0f mV %s" % (v, m[0], m[1], err * 1000, "" if good else "FAIL"))
    i, n = marks["sine"]
    y = seg(i, n)
    for c in range(2):
        s = y[:, c] - y[:, c].mean()
        rms = np.sqrt(np.mean(s ** 2))
        spec = np.abs(np.fft.rfft(s * np.hanning(len(s))))
        f = np.fft.rfftfreq(len(s), 1 / FS)
        k = np.argmax(spec)
        band = (f > f[k] * 0.9) & (f < f[k] * 1.1)
        thdn = np.sqrt(np.sum(spec[~band] ** 2) / np.sum(spec[band] ** 2))
        print("  sine in%d: %.1f Hz, %.3f Vrms (expect %.3f), THD+N %.2f%%" % (c + 1, f[k], rms, 2.5 / np.sqrt(2), thdn * 100))
    i, n = marks["ramp"]
    y = seg(i, n, settle=0.02)
    jumps = np.abs(np.diff(y[:, 0]))
    step = 7.0 / n
    worst = jumps.max()
    if worst >= 0.05:
        print("  worst jump at ramp frame %d of %d" % (np.argmax(jumps), len(jumps)))
    good = worst < 0.05
    ok &= good
    print("ramp: %d frames, expected step %.2f mV, worst jump %.1f mV %s" % (len(y), step * 1000, worst * 1000, "" if good else "FAIL (dropout?)"))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
