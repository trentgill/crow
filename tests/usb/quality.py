#!/usr/bin/env python3
"""Signal quality of crow's USB audio loop, out1 -> in1 & out2 -> in2 patched.

    python3 tests/usb/quality.py --card 5

Plays steady tones through ALSA (hw:<card>,0) and records the inputs. Every
figure is for the whole loop: USB -> DAC -> jack -> ADC -> USB.
- frequency response (0.25 = 2.5 V peak)
- THD (harmonics 2-9) and THD+N at 1 kHz for several levels
- idle noise, and the SNR against the largest clean sine (7.5 V peak, the
  -5..+10 V range centred on +2.5 V)
"""
import argparse, os, subprocess, sys, tempfile, time, wave
import numpy as np

FS = 48000

def loop(card, out, extra=1.0):
    d = tempfile.mkdtemp(); p, r = os.path.join(d, "p.wav"), os.path.join(d, "r.wav")
    w = wave.open(p, "wb"); w.setnchannels(4); w.setsampwidth(2); w.setframerate(FS)
    w.writeframes((np.clip(out, -1, 32767 / 32768) * 32768).astype("<i2").tobytes()); w.close()
    rec = subprocess.Popen(["arecord", "-q", "-D", "hw:%s,0" % card, "-c", "2", "-f", "S16_LE",
                            "-r", str(FS), "-d", str(int(len(out) / FS + extra + 0.5)), r])
    time.sleep(0.3)
    subprocess.run(["aplay", "-q", "-D", "hw:%s,0" % card, p], check=True)
    rec.wait()
    w = wave.open(r)
    return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2) / 32768.0 * 10.0

def tone(f, amp, secs=1.5, dc=0.0):
    t = np.arange(int(FS * secs)) / FS
    x = np.zeros((len(t), 4)); x[:, 0] = x[:, 1] = dc + amp * np.sin(2 * np.pi * f * t)
    return x

def steady(y, f, amp_v):
    """the middle of the loudest stretch: where the tone is"""
    env = np.convolve(np.abs(y - np.median(y)), np.ones(480) / 480, "same")
    on = np.nonzero(env > 0.5 * env.max())[0] # however much the loop lost
    a, b = on[0], on[-1]
    m = (b - a) // 6
    seg = y[a + m:b - m]
    n = 2 ** int(np.log2(len(seg)))
    return seg[:n]

def analyse(seg, f):
    s = seg - seg.mean()
    win = np.blackman(len(s))
    spec = np.abs(np.fft.rfft(s * win)) ** 2
    fr = np.fft.rfftfreq(len(s), 1 / FS)
    def band(fc, w=4):
        k = int(round(fc / (FS / len(s))))
        return spec[max(k - w, 1):k + w + 1].sum(), range(max(k - w, 1), k + w + 1)
    p1, b1 = band(f)
    harm = 0.0; hb = set()
    for h in range(2, 10):
        if h * f < FS / 2 - 200:
            ph, bh = band(h * f); harm += ph; hb |= set(bh)
    rest = spec.copy(); rest[:3] = 0; rest[list(b1)] = 0
    thd = np.sqrt(harm / p1); thdn = np.sqrt(rest.sum() / p1)
    rms = np.sqrt(np.mean(s ** 2))
    return rms, thd, thdn

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--card", required=True)
    a = ap.parse_args()
    print("frequency response, 2.5 V peak (expect %.3f Vrms)" % (2.5 / np.sqrt(2)))
    ref = None
    for f in (20, 50, 100, 1000, 5000, 10000, 15000, 18000, 20000, 22000):
        y = loop(a.card, tone(f, 0.25))
        rms = [analyse(steady(y[:, c], f, 2.5), f)[0] for c in range(2)]
        if f == 1000: ref = rms
        print("  %6d Hz  in1 %.3f Vrms (%+.2f dB)  in2 %.3f Vrms (%+.2f dB)" %
              (f, rms[0], 20 * np.log10(rms[0] / (2.5 / np.sqrt(2))), rms[1], 20 * np.log10(rms[1] / (2.5 / np.sqrt(2)))))
    print("THD at 1 kHz")
    for amp, dc in ((0.05, 0), (0.25, 0), (0.45, 0), (0.75, 0.25)):
        y = loop(a.card, tone(1000, amp, dc=dc))
        for c in range(2):
            rms, thd, thdn = analyse(steady(y[:, c], 1000, amp * 10), 1000)
            print("  %.1f V peak%s in%d: %.3f Vrms  THD %.3f%% (%.1f dB)  THD+N %.3f%% (%.1f dB)" %
                  (amp * 10, " on +2.5 V" if dc else "", c + 1, rms, thd * 100, 20 * np.log10(thd), thdn * 100, 20 * np.log10(thdn)))
    print("idle (0 V out)")
    y = loop(a.card, np.zeros((FS * 3, 4)))[FS // 2:-FS // 2]
    for c in range(2):
        s = y[:, c] - y[:, c].mean()
        n = np.sqrt(np.mean(s ** 2))
        # 20 Hz - 20 kHz band only
        spec = np.abs(np.fft.rfft(s)) ** 2; fr = np.fft.rfftfreq(len(s), 1 / FS)
        nb = np.sqrt(spec[(fr >= 20) & (fr <= 20000)].sum() / spec[1:].sum()) * n
        print("  in%d: offset %+.2f mV, noise %.3f mVrms (%.3f mVrms 20 Hz-20 kHz)" % (c + 1, y[:, c].mean() * 1000, n * 1000, nb * 1000))
        print("       SNR vs 7.5 V peak sine: %.1f dB;  vs 5 V peak: %.1f dB;  16 bit LSB = 0.305 mV" %
              (20 * np.log10(7.5 / np.sqrt(2) / nb), 20 * np.log10(5 / np.sqrt(2) / nb)))

if __name__ == "__main__":
    main()
