#!/usr/bin/env python3
"""Round-trip latency and noise of crow's USB audio, out1 -> in1 patched.

    python3 tests/usb/roundtrip.py --card 5 [--period 48] [--periods 2]

Opens playback (4ch) and capture (2ch) on hw:<card>,0 through libasound (by
ctypes: no dev headers needed), links them so they start on the same frame,
keeps the playback buffer full, and plays clicks on output 1. The lag at which
each click comes back on input 1 is the round trip a duplex app sees at this
buffer size: playback buffer + crow's out path + the analog loop + crow's in
path + the capture side's packet. It also records silence for the noise floor.
"""
import argparse, ctypes, ctypes.util, sys
import numpy as np

FS = 48000
asound = ctypes.CDLL(ctypes.util.find_library("asound") or "libasound.so.2")
SND_PCM_STREAM_PLAYBACK, SND_PCM_STREAM_CAPTURE = 0, 1
SND_PCM_FORMAT_S16_LE = 2
SND_PCM_ACCESS_RW_INTERLEAVED = 3
asound.snd_strerror.restype = ctypes.c_char_p
asound.snd_pcm_writei.restype = ctypes.c_long
asound.snd_pcm_readi.restype = ctypes.c_long
asound.snd_pcm_writei.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
asound.snd_pcm_readi.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]

def chk(r, what):
    if r < 0:
        sys.exit("%s: %s" % (what, asound.snd_strerror(r).decode()))
    return r

class Pcm:
    def __init__(self, dev, stream, ch, period, periods):
        self.h = ctypes.c_void_p()
        chk(asound.snd_pcm_open(ctypes.byref(self.h), dev.encode(), stream, 0), "open " + dev)
        self.ch = ch
        hw = ctypes.c_void_p()
        chk(asound.snd_pcm_hw_params_malloc(ctypes.byref(hw)), "hw malloc")
        chk(asound.snd_pcm_hw_params_any(self.h, hw), "any")
        chk(asound.snd_pcm_hw_params_set_access(self.h, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "access")
        chk(asound.snd_pcm_hw_params_set_format(self.h, hw, SND_PCM_FORMAT_S16_LE), "format")
        chk(asound.snd_pcm_hw_params_set_channels(self.h, hw, ch), "channels")
        chk(asound.snd_pcm_hw_params_set_rate(self.h, hw, FS, 0), "rate")
        p = ctypes.c_ulong(period)
        chk(asound.snd_pcm_hw_params_set_period_size_near(self.h, hw, ctypes.byref(p), None), "period")
        b = ctypes.c_ulong(period * periods)
        chk(asound.snd_pcm_hw_params_set_buffer_size_near(self.h, hw, ctypes.byref(b)), "buffer")
        chk(asound.snd_pcm_hw_params(self.h, hw), "hw params")
        self.period, self.buffer = p.value, b.value
        asound.snd_pcm_hw_params_free(hw)
        sw = ctypes.c_void_p()
        asound.snd_pcm_sw_params_malloc(ctypes.byref(sw))
        asound.snd_pcm_sw_params_current(self.h, sw)
        # playback starts by itself only when the buffer is full (it's linked)
        asound.snd_pcm_sw_params_set_start_threshold(self.h, sw, ctypes.c_ulong(self.buffer))
        chk(asound.snd_pcm_sw_params(self.h, sw), "sw params")
        asound.snd_pcm_sw_params_free(sw)

    def write(self, x):
        a = np.ascontiguousarray((np.clip(x, -1, 32767 / 32768) * 32768).astype("<i2"))
        n = chk(asound.snd_pcm_writei(self.h, a.ctypes.data, len(a)), "write")
        return n

    def read(self, n):
        a = np.zeros((n, self.ch), "<i2")
        got = chk(asound.snd_pcm_readi(self.h, a.ctypes.data, n), "read")
        return a[:got] / 32768.0

def run(card, period, periods, seconds, clicks):
    dev = "hw:%s,0" % card
    pb = Pcm(dev, SND_PCM_STREAM_PLAYBACK, 4, period, periods)
    # capture gets room to spare: its buffer size adds no lag (frames are
    # counted from the linked start), it only stops overruns
    cp = Pcm(dev, SND_PCM_STREAM_CAPTURE, 2, period, max(periods, 16))
    chk(asound.snd_pcm_link(pb.h, cp.h), "link")
    total = int(FS * seconds)
    out = np.zeros((total, 4))
    marks = []
    if clicks:
        # a half-sine click of 1 ms, ±2.5 V, every 0.25 s
        w = np.sin(np.linspace(0, np.pi, 48)) * 0.25
        for i in range(FS // 2, total - FS // 4, FS // 4):
            out[i:i + 48, 0] = w
            marks.append(i)
    rec = []
    pos = 0
    # prefill: the linked pair starts when the playback buffer is full
    pos += pb.write(out[pos:pos + pb.buffer])
    while pos < total:
        n = min(pb.period, total - pos)
        rec.append(cp.read(pb.period))
        pos += pb.write(out[pos:pos + n])
    for _ in range(periods + 8): # keep playback fed: an underrun stops both
        rec.append(cp.read(pb.period))
        pb.write(np.zeros((pb.period, 4)))
    asound.snd_pcm_drop(pb.h); asound.snd_pcm_drop(cp.h)
    asound.snd_pcm_close(pb.h); asound.snd_pcm_close(cp.h)
    return pb, np.concatenate(rec), out, marks

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", required=True)
    ap.add_argument("--period", type=int, default=48)
    ap.add_argument("--periods", type=int, default=2)
    ap.add_argument("--seconds", type=float, default=4.0)
    a = ap.parse_args()

    pb, x, out, marks = run(a.card, a.period, a.periods, a.seconds, True)
    print("buffer %d frames (%d x %d), %.2f ms" % (pb.buffer, pb.periods if hasattr(pb, 'periods') else a.periods,
                                                  pb.period, pb.buffer / FS * 1000))
    y = x[:, 0] - np.median(x[:, 0])
    lags = []
    for m in marks:
        seg = y[m:m + FS // 5]
        if len(seg) < 100: continue
        k = int(np.argmax(seg))
        if seg[k] < 0.1: continue  # no click back (> 1 V)
        # peak of the returned click vs. the peak of the sent one (24 in)
        lags.append(k - 24)
    if not lags:
        sys.exit("no clicks came back on input 1: is out1 patched to in1?")
    lags = np.array(lags)
    print("round trip: median %.0f frames = %.2f ms (min %.2f, max %.2f, %d clicks)" %
          (np.median(lags), np.median(lags) / FS * 1000, lags.min() / FS * 1000, lags.max() / FS * 1000, len(lags)))
    print("  of which the playback buffer kept full: %d frames = %.2f ms" % (pb.buffer, pb.buffer / FS * 1000))

    # noise floor: silence out, record
    _, x, _, _ = run(a.card, a.period, a.periods, 3.0, False)
    for c in range(2):
        s = x[FS // 2:, c] * 10.0
        print("  in%d idle: mean %+.2f mV, %.3f mVrms noise, %.1f mV p-p" %
              (c + 1, s.mean() * 1000, s.std() * 1000, (s.max() - s.min()) * 1000))
    return 0

if __name__ == "__main__":
    sys.exit(main())
