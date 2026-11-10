"""Drive the desktop simulator from a script (for testing UI changes).

usage: python tools/sim_drive.py out_dir "cmd" "cmd" ... "@shot name.bmp" ...
Commands are sent to the simulator's console one by one. "wait N" pauses N ms.
Lines starting with @ go to the simulator itself (@shot, @quit); the rest go to
the firmware's USB console (cw, ccw, press, long, double, swipe l, status ...).
Screenshots are saved into out_dir as PNG.
"""
import os, subprocess, sys, time
from PIL import Image

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.join(root, "sim", "build", "SorentoSim.exe")
out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
time.sleep(2.5)  # boot
shots = []
for c in sys.argv[2:]:
    if c.startswith("wait "):
        time.sleep(int(c.split()[1]) / 1000)
        continue
    if c.startswith("@shot "):
        name = c.split(" ", 1)[1]
        bmp = os.path.join(out, name if name.endswith(".bmp") else name + ".bmp")
        c = "@shot " + bmp
        shots.append(bmp)
    p.stdin.write(c + "\n")
    p.stdin.flush()
    time.sleep(0.03)  # keep timing-sensitive input (double-clicks) intact
p.stdin.write("@quit\n")
p.stdin.flush()
try:
    log, _ = p.communicate(timeout=10)
except subprocess.TimeoutExpired:
    p.kill()
    log, _ = p.communicate()
time.sleep(0.2)
for bmp in shots:
    if os.path.exists(bmp):
        png = bmp[:-4] + ".png"
        Image.open(bmp).save(png)
        os.remove(bmp)
        print("saved", png)
print(log[-1500:])
