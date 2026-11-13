"""Flash crow.bin to both bench crows: A over its ST-Link, B over USB DFU.

    python3 tests/ii/flash.py [crow.bin] [--only A|B]
"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "hw"))
import crowhw  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
A_SN = os.environ.get("CROW_A", "206135773756")
B_SN = os.environ.get("CROW_B", "206635743756")
STLINK = os.environ.get("CROW_STLINK", "066BFF383834434257094241")


def flash_a(binfile):
    c = crowhw.Crow(usb_serial=A_SN, stlink=STLINK, settle=0.3)
    c.flash(binfile)
    print("A:", c.command("^^v", idle=0.3))
    c.close()


def flash_b(binfile):
    c = crowhw.Crow(usb_serial=B_SN, settle=0.3).open()
    c.write("^^b")
    c.close()
    t0 = time.monotonic()
    while subprocess.run(["lsusb", "-d", "0483:df11"], capture_output=True).returncode:
        if time.monotonic() - t0 > 10:
            raise SystemExit("crow B did not enter DFU")
        time.sleep(0.2)
    time.sleep(0.5)
    subprocess.run([sys.executable, os.path.join(HERE, "dfuse.py"), "write", binfile], check=True,
                   stdout=subprocess.DEVNULL)
    c = crowhw.Crow(usb_serial=B_SN, settle=0.5)
    c.wait_port(timeout=15)
    c.open()
    print("B:", c.command("^^v", idle=0.3))
    c.close()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    binfile = args[0] if args else os.path.join(HERE, "..", "..", "crow.bin")
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else "AB"
    # B first: a crow that boots while the ii bus is held low (SCL low) starts
    # its bootloader instead of the app, and a stuck B can be what holds it
    if "B" in only:
        flash_b(binfile)
    if "A" in only:
        flash_a(binfile)
