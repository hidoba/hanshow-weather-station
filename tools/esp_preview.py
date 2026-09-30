#!/usr/bin/env python3
"""Grab the ESP32's current framebuffer over USB ('d' command) and save it as a PNG (scaled 3x)."""
import sys, time, serial
from PIL import Image

W, H, ROW = 250, 122, 16
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
out = sys.argv[2] if len(sys.argv) > 2 else "preview.png"
s = serial.Serial(port, 115200, timeout=0.5)
s.write(b"d")
t0 = time.time()
while time.time() - t0 < 10:
    line = s.readline().decode(errors="replace").strip()
    if line.startswith("PLANES "):
        data = bytes.fromhex(line[7:])
        break
else:
    sys.exit("no dump received")
black, red = data[:ROW * W], data[ROW * W:]
img = Image.new("RGB", (W, H), "white")
px = img.load()
for x in range(W):
    for y in range(H):
        i, m = (y >> 3) + (W - 1 - x) * ROW, 0x80 >> (y & 7)
        if red[i] & m: px[x, y] = (200, 20, 20)
        elif not black[i] & m: px[x, y] = (0, 0, 0)
img.resize((W * 3, H * 3), Image.NEAREST).save(out)
print("saved", out)
