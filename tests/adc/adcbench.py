"""ADS131 sync experiments on the bench crow. Uses tests/hw/crowhw.py."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'hw'))
import subprocess
import crowhw
from crowhw import Crow as _Crow

# prism can have more than one ST-Link attached: always name crow's (V2, F72x)
STLINK = "066BFF383834434257094241"
OPENOCD = ["openocd", "-f", "interface/stlink.cfg", "-c", "adapter serial " + STLINK,
           "-f", "target/stm32f7x.cfg"]

class Crow(_Crow):
    def reset(self, settle=True):
        self.close()
        subprocess.run(["st-flash", "--serial", STLINK, "reset"], capture_output=True, check=True, timeout=30)
        _, dt = self.wait_port(gone_first=True)
        self.open(settle=settle)
        self._boot_text()
        return dt

    def flash(self, binfile):
        self.close()
        r = subprocess.run(["st-flash", "--serial", STLINK, "--reset", "write", binfile, crowhw.APP_ADDRESS],
                           capture_output=True, text=True, timeout=120)
        out = r.stderr + r.stdout
        if "jolly good" not in out or "STM32F72x" not in out:
            raise RuntimeError("st-flash failed:\n" + out[-800:])
        self.wait_port(gone_first=True)
        self.open()
        self._boot_text()

def dbg(c):
    code = ("do local t=adc_debug() local s={} for k,v in pairs(t) do "
            "s[#s+1]=k..'='..tostring(v) end print('#ad '..table.concat(s,' ')) end")
    for l in c.lua(code):
        if l.startswith('#ad '):
            d = {}
            for kv in l[4:].split():
                k, v = kv.split('=', 1)
                d[k] = float(v)
            return d
    raise RuntimeError('no adc_debug output')

def setcfg(c, **kw):
    c.lua(';'.join("adc_debug('set','%s',%d)" % (k, v) for k, v in kw.items()))

def apply(c):
    c.lua("adc_debug('apply')", timeout=5)

def window(c, secs):
    c.lua("adc_debug('reset')")
    time.sleep(secs)
    return dbg(c)

def connect():
    c = Crow()
    c.open()
    c._boot_text()
    return c

def capture(c, ch=1, n=1024):
    c.lua("adc_debug('capture',%d,%d)" % (ch, n))
    time.sleep(n / 46875 * 1.5 + 0.1)
    while int(c.eval("adc_debug('captured')")) < n: time.sleep(0.05)
    vals = []
    for i in range(0, n, 128):
        out = c.lua("print('#cap '..adc_debug('captured',%d,128))" % i)
        for l in out:
            if l.startswith('#cap '):
                vals += [int(x) for x in l[5:].split(',') if x]
    return vals
