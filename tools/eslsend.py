#!/usr/bin/env python3
"""Send a 250x122 image to the Hanshow tag running hanshow_serial firmware.

  eslsend.py ping
  eslsend.py test                # render a test card
  eslsend.py image foo.png       # any image, resized/quantized to black/white/red
"""
import argparse, struct, sys, time
import serial
from PIL import Image, ImageDraw, ImageFont

W, H, ROW = 250, 122, 16


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def to_planes(img):
    img = img.convert("RGB").resize((W, H))
    black = bytearray([0xFF] * ROW * W)
    red = bytearray(ROW * W)
    px = img.load()
    for x in range(W):
        for y in range(H):
            r, g, b = px[x, y]
            i = (y >> 3) + (W - 1 - x) * ROW
            m = 0x80 >> (y & 7)
            if r > 150 and g < 110 and b < 110:
                red[i] |= m
            elif (r * 3 + g * 6 + b) / 10 < 128:
                black[i] &= ~m & 0xFF
    return black, red


class Tag:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, baud, 1
        self.s.dtr = self.s.rts = False  # RTS is wired to NRST on the flashing adapter: don't reset on open
        self.s.open()
        # Linux pulses RTS on open anyway, which resets the tag: wait for it to boot
        t0 = time.time()
        while time.time() - t0 < 3:
            if b"READY" in self.s.readline():
                break

    def cmd(self, c, payload=b"", timeout=2.0):
        body = bytes([ord(c)]) + struct.pack("<H", len(payload)) + payload
        self.s.reset_input_buffer()
        self.s.write(b"\xA5\x5A" + body + struct.pack("<H", crc16(body)))
        t0 = time.time()
        while time.time() - t0 < timeout:
            line = self.s.readline().decode(errors="replace").strip()
            if line.startswith(("OK", "ER")):
                return line
            if line:
                print("  tag:", line)
        raise TimeoutError(f"no reply to {c!r}")

    def send_planes(self, black, red, chunk=240):
        for plane, data in ((0, black), (1, red)):
            for off in range(0, len(data), chunk):
                for attempt in range(3):
                    r = self.cmd("W", bytes([plane]) + struct.pack("<H", off) + data[off:off + chunk])
                    if r == "OK W":
                        break
                else:
                    raise RuntimeError(f"write failed at plane {plane} off {off}: {r}")
        return self.cmd("D", timeout=40)


def test_card():
    img = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(img)
    try:
        big = ImageFont.truetype("DejaVuSans-Bold.ttf", 30)
        small = ImageFont.truetype("DejaVuSans.ttf", 14)
    except OSError:
        big = small = ImageFont.load_default()
    d.rectangle([0, 0, W - 1, H - 1], outline="black", width=2)
    d.rectangle([0, 0, W - 1, 30], fill="red")
    d.text((8, 5), "HANSHOW + ESP32", font=small, fill="white")
    d.text((10, 40), "Hello! 21°C", font=big, fill="black")
    d.text((10, 80), time.strftime("%Y-%m-%d %H:%M"), font=small, fill="red")
    for i in range(8):
        d.rectangle([180 + i * 8, 80, 185 + i * 8, 115], fill="black" if i % 2 else "red")
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["ping", "test", "image"])
    ap.add_argument("file", nargs="?")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    a = ap.parse_args()
    tag = Tag(a.port)
    if a.cmd == "ping":
        print(tag.cmd("P"))
        return
    img = test_card() if a.cmd == "test" else Image.open(a.file)
    img.save("/tmp/eslsend_last.png") if a.cmd == "test" else None
    t0 = time.time()
    print(tag.send_planes(*to_planes(img)), f"({time.time() - t0:.1f}s)")


if __name__ == "__main__":
    main()
