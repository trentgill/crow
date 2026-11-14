"""Minimal DfuSe client for crow's bootloader (no dfu-util on this machine).

    python3 tests/ii/dfuse.py read  out.bin [addr] [length]   # default: all 512 KB
    python3 tests/ii/dfuse.py write crow.bin [addr]          # default: app at 0x08020000
    python3 tests/ii/dfuse.py leave                         # just start the app

Refuses to run unless exactly one STM DFU device (0483:df11) is attached.
Write erases only the sectors the image covers, then reads back and compares.
"""
import os, struct, sys, time
import usb.core, usb.util

VID, PID = 0x0483, 0xdf11
DNLOAD, UPLOAD, GETSTATUS, CLRSTATUS, ABORT = 1, 2, 3, 4, 6
IDLE, DL_BUSY, DL_IDLE, UP_IDLE, MANIFEST = 2, 4, 5, 9, 7
FLASH = 0x08000000
# STM32F722 sectors: 4x16K, 64K, 3x128K
SECTORS = [(FLASH + o, s) for o, s in
           [(0, 16 << 10), (16 << 10, 16 << 10), (32 << 10, 16 << 10), (48 << 10, 16 << 10),
            (64 << 10, 64 << 10), (128 << 10, 128 << 10), (256 << 10, 128 << 10), (384 << 10, 128 << 10)]]
APP = 0x08020000


class Dfu:
    def __init__(self):
        devs = list(usb.core.find(idVendor=VID, idProduct=PID, find_all=True))
        if os.environ.get("DFU_BUS"):  # pick one by USB bus number when several are attached
            devs = [d for d in devs if d.bus == int(os.environ["DFU_BUS"])]
        if len(devs) != 1:
            raise SystemExit("need exactly one DFU device, found %d" % len(devs))
        self.d = devs[0]
        self.xfer = 2048
        for cfg in self.d:
            for intf in cfg:
                ex = bytes(intf.extra_descriptors)
                if len(ex) >= 7 and ex[1] == 0x21:
                    self.xfer = struct.unpack_from("<H", ex, 5)[0]
        usb.util.claim_interface(self.d, 0)
        self.abort()
        while self.status()[4] != IDLE:
            self.d.ctrl_transfer(0x21, CLRSTATUS, 0, 0, None, 5000)

    def status(self):
        s = self.d.ctrl_transfer(0xA1, GETSTATUS, 0, 0, 6, 20000)
        poll = s[1] | (s[2] << 8) | (s[3] << 16)
        if poll:
            time.sleep(poll / 1000)
        return s

    def abort(self):
        self.d.ctrl_transfer(0x21, ABORT, 0, 0, None, 5000)

    def _cmd(self, payload, block=0):
        self.d.ctrl_transfer(0x21, DNLOAD, block, 0, payload, 5000)
        s = self.status()            # executes (busy)
        while s[4] == DL_BUSY:
            s = self.status()
        if s[4] != DL_IDLE or s[0] != 0:
            raise RuntimeError("dfu command failed: status %d state %d" % (s[0], s[4]))

    def set_address(self, a):
        self._cmd(struct.pack("<BI", 0x21, a))

    def erase(self, a):
        self._cmd(struct.pack("<BI", 0x41, a))

    def read(self, addr, length):
        self.set_address(addr)
        self.abort()
        out = bytearray()
        n = 0
        while len(out) < length:
            want = min(self.xfer, length - len(out))
            out += bytes(self.d.ctrl_transfer(0xA1, UPLOAD, 2 + n, 0, self.xfer, 20000))[:want]
            n += 1
        self.abort()
        return bytes(out)

    def write(self, addr, data):
        for i in range(0, len(data), self.xfer):
            self.set_address(addr + i)
            self._cmd(data[i:i + self.xfer], block=2)

    def leave(self, addr):
        self.set_address(addr)
        self.d.ctrl_transfer(0x21, DNLOAD, 0, 0, None, 5000)
        try:
            self.status()
        except usb.core.USBError:
            pass


def main():
    op, path = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "-")
    d = Dfu()
    print("transfer size", d.xfer)
    if op == "read":
        addr = int(sys.argv[3], 0) if len(sys.argv) > 3 else FLASH
        length = int(sys.argv[4], 0) if len(sys.argv) > 4 else 512 << 10
        open(path, "wb").write(d.read(addr, length))
        print("read %d bytes from 0x%08x" % (length, addr))
    elif op == "leave":
        d.leave(int(path, 0) if path != "-" else APP)
    elif op == "write":
        addr = int(sys.argv[3], 0) if len(sys.argv) > 3 else APP
        if addr < APP:
            raise SystemExit("refusing to write below the app (bootloader, calibration, script)")
        data = open(path, "rb").read()
        for base, size in SECTORS:
            if base < addr + len(data) and base + size > addr:
                if base < addr:
                    raise SystemExit("image does not start on a sector boundary")
                print("erase 0x%08x" % base)
                d.erase(base)
        d.write(addr, data)
        back = d.read(addr, len(data))
        if back != data:
            raise SystemExit("VERIFY FAILED")
        print("verified %d bytes at 0x%08x" % (len(data), addr))
        d.leave(addr)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
