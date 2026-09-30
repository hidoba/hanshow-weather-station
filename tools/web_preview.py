#!/usr/bin/env python3
"""Save what the display currently shows as a PNG, fetched from the ESP32 over Wi-Fi.

  web_preview.py [http://weather.local] [out.png]
"""
import sys
import urllib.request
from PIL import Image

W, H, ROW = 250, 122, 16
base = sys.argv[1] if len(sys.argv) > 1 else "http://weather.local"
out = sys.argv[2] if len(sys.argv) > 2 else "preview.png"
data = urllib.request.urlopen(base.rstrip("/") + "/planes", timeout=10).read()
black, red = data[:ROW * W], data[ROW * W:]
img = Image.new("RGB", (W, H), "white")
px = img.load()
for x in range(W):
    for y in range(H):
        i, m = (y >> 3) + (W - 1 - x) * ROW, 0x80 >> (y & 7)
        if red[i] & m:
            px[x, y] = (200, 20, 20)
        elif not black[i] & m:
            px[x, y] = (0, 0, 0)
img.resize((W * 3, H * 3), Image.NEAREST).save(out)
print("saved", out)
