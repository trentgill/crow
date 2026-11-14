"""Check the ADC-to-DAC resampler: an oscillator from output 1, looped back to
input 1 with cal.source(1), captured raw (46.875 kHz) and as the block sees it
(the DAC rate). Both should fit the same frequency with similar residuals.

    python3 tests/adc/resample.py
"""
import sys, os, time
import numpy as np
import adcbench as a
c = a.connect()
c.lua("cal.source(1)")
FS_ADC = 46875.0
FS_DAC = 172e6 / 3584  # PLLI2S 344/2, 3584 I2S clocks per sample
def fit(x, fs, f0):
    n = np.arange(len(x)); x = x - x.mean()
    best=None
    for f in np.linspace(f0*0.97, f0*1.03, 3001):
        w = 2*np.pi*f/fs
        A = np.vstack([np.sin(w*n), np.cos(w*n), np.ones_like(n)]).T
        co, res, *_ = np.linalg.lstsq(A, x, rcond=None)
        r = x - A@co
        e = np.sqrt(np.mean(r**2))
        if best is None or e < best[1]: best=(f,e,np.hypot(co[0],co[1]),r)
    return best
for f in (97, 1000, 5000):
    c.lua("output[1].action=oscillate(%d,3) output[1]()" % f); time.sleep(0.5)
    for ch, fs in ((1, FS_ADC), (3, FS_DAC)):
        x = np.array(a.capture(c, ch, 8192), float)
        fb, e, amp, r = fit(x, fs, f*1.0)
        print('osc %5d ch%d  f=%.2f Hz  amp=%.0f  resid_rms=%.1f LSB  max|resid|=%.0f' % (f, ch, fb, amp, e, np.abs(r).max()), flush=True)
c.lua("output[1].action=to(0,0) output[1]() cal.source('gnd')")
