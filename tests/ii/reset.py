"""Reset both bench crows without touching eurorack power.

A crow that boots while SCL is held low starts its bootloader instead of the
app, so with a stuck bus the order matters: B goes into its bootloader first
(^^b), which lets go of the bus; A is reset over its ST-Link and boots clean;
then B leaves the bootloader (DfuSe 'leave').
"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash  # noqa: E402
import crowhw  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def b_to_dfu():
    c = crowhw.Crow(usb_serial=flash.B_SN, settle=0.3).open()
    c.write("^^b")
    c.close()
    t0 = time.monotonic()
    while subprocess.run(["lsusb", "-d", "0483:df11"], capture_output=True).returncode:
        if time.monotonic() - t0 > 10:
            raise SystemExit("crow B did not enter DFU")
        time.sleep(0.2)
    time.sleep(0.5)


def b_leave():
    subprocess.run([sys.executable, os.path.join(HERE, "dfuse.py"), "leave"], check=True,
                   stdout=subprocess.DEVNULL)
    crowhw.Crow(usb_serial=flash.B_SN).wait_port(timeout=15)


def reset_a():
    c = crowhw.Crow(usb_serial=flash.A_SN, stlink=flash.STLINK, settle=0.3)
    c.reset()
    c.close()


if __name__ == "__main__":
    b_to_dfu()
    reset_a()
    b_leave()
    print("both reset")
