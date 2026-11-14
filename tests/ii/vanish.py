"""A leader that vanishes mid-transfer: crow A is reset over its ST-Link 10
times while it queries W/, and crow B keeps querying W/ throughout.
Counts how often A boots into its bootloader (SCL low at the instant it looks).
"""
# a leader (crow A) is reset mid-traffic, 10 times, while crow B keeps querying W/
import sys, time, random, subprocess
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import c2c, crowhw
A, B = c2c.open_crow(c2c.A_SN), c2c.open_crow(c2c.B_SN)
c2c.load(B, 2, 1)
B.lua("ii.wtape.speed(1)"); time.sleep(0.3)
B.lua("ii.stats('reset')")
B.write("runw(30000,0.002,1,'wq',1)\r\n")
dfu = 0
for i in range(10):
    c2c.load(A, 1, 2)
    A.lua("ii.stats('reset')")
    A.write("runw(100000,0.001,2,'wq',1)\r\n")
    time.sleep(random.uniform(0.3, 1.0))
    A.close()
    r = subprocess.run(["st-flash", "--serial", "066BFF383834434257094241", "reset"], capture_output=True, text=True)
    A = crowhw.Crow(usb_serial=c2c.A_SN, stlink="066BFF383834434257094241", settle=0.3)
    try:
        A.wait_port(gone_first=True, timeout=4)
    except TimeoutError:
        # booted into its bootloader: SCL was low (bus traffic) at the instant it looked
        dfu += 1
        print(i, "A started its bootloader; leaving it", flush=True)
        subprocess.run([sys.executable, __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'dfuse.py'), "leave"], env=dict(__import__("os").environ, DFU_BUS="7"), capture_output=True)
        A.wait_port(timeout=15)
    A.open()
    A._boot_text()
    print(i, "A back:", A.command("^^v", idle=0.3)[:1], flush=True)
time.sleep(5)
d = c2c.parse(B.lua("reportw()", timeout=5))
print("A entered its bootloader", dfu, "of 10 resets")
print("B: queries", d.get("W.qsent"), "ok", d.get("W.ok"), "bad", d.get("W.bad"), "q_full", d.get("q_full"), "nack_drop", d.get("nack_drop"), "recover", d.get("recover"), "abandon", d.get("abandon"), "busy_max", d.get("busy_max_us"))
