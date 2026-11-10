"""Development helper: send console commands to the board and grab screenshots.

usage: python shot.py [--port COM3] out.png [cmd ...]
Each cmd (e.g. "cw 2", "press", "long", "wait 500") is sent before the shot.
"""
import sys, time, serial
from PIL import Image, ImageDraw

args = sys.argv[1:]
port = "COM3"
if args[:1] == ["--port"]:
    port, args = args[1], args[2:]
out, cmds = args[0], args[1:]

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 3
s.dtr = s.rts = False  # don't reset the board
s.open()
s.reset_input_buffer()
for c in cmds:
    if c.startswith("wait"):
        time.sleep(int(c.split()[1]) / 1000)
    else:
        s.write((c + "\n").encode())
        time.sleep(0.15)
time.sleep(0.4)  # let animations settle
s.reset_input_buffer()
s.write(b"shot\n")
buf = b""
deadline = time.time() + 6
while b"SHOT240\n" not in buf and time.time() < deadline:
    buf += s.read(s.in_waiting or 1)
data = buf.split(b"SHOT240\n", 1)[1]
while len(data) < 240 * 240 * 2 and time.time() < deadline:
    data += s.read(240 * 240 * 2 - len(data))
s.close()

img = Image.new("RGB", (240, 240))
px = []
for i in range(0, 240 * 240 * 2, 2):
    v = (data[i] << 8) | data[i + 1]  # LovyanGFX sprites are big-endian RGB565
    px.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
img.putdata(px)
# Mask to the round panel and upscale for easier viewing.
mask = Image.new("L", (240, 240), 0)
ImageDraw.Draw(mask).ellipse((0, 0, 239, 239), fill=255)
bg = Image.new("RGB", (240, 240), (40, 40, 40))
bg.paste(img, (0, 0), mask)
bg.resize((480, 480), Image.NEAREST).save(out)
print("saved", out)
