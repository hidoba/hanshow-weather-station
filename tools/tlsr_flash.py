#!/usr/bin/env python3
"""Flash a Telink TLSR825x (Hanshow ESL) over SWS using a plain USB-UART adapter.

Python port of atc1441's ATC_TLSR_Paper_UART_Flasher.html (pvvx swire-over-uart trick),
https://github.com/atc1441/ATC_TLSR_Paper, MIT license (see firmware/tag/LICENSE-ATC_TLSR_Paper).
Wiring: UART TX -> SWS (+RXD), RTS -> NRST, GND, 3V3.
"""
import argparse, sys, time
import serial


def _enc_byte(b, first):
    d = bytearray(10)
    d[0] = 0x80 if first else 0xFE          # swire start bit: 1 for cmd, 0 for data
    for i in range(8):
        d[1 + i] = 0x80 if (b >> (7 - i)) & 1 else 0xFE
    d[9] = 0xFE                              # swire stop bit = 0
    return d


def sws_wr_addr(addr, data):
    hdr = bytes([0x5A, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, 0x00])
    pkt = bytearray()
    for n, b in enumerate(hdr):
        pkt += _enc_byte(b, n == 0)
    for b in data:
        pkt += _enc_byte(b, False)
    pkt += bytes([0x80] * 9 + [0xFE])        # swire end cmd = 0xff
    return bytes(pkt)


class Swire:
    def __init__(self, port, baud):
        self.s = serial.Serial(port, baud, timeout=0.1)
        self.s.dtr = False
        self.s.rts = False

    def wr(self, addr, data):
        self.s.write(sws_wr_addr(addr, bytes(data)))

    def flush(self):
        self.s.flush()

    def flash_byte_cmd(self, cmd):
        self.wr(0x0D, [0x00])                # cns low
        self.wr(0x0C, [cmd & 0xFF, 0x01])    # cmd + cns high

    def write_enable(self):
        self.flash_byte_cmd(0x06)

    def unlock(self):
        self.write_enable()
        self.wr(0x0D, [0x00]); self.wr(0x0C, [0x01]); self.wr(0x0C, [0x00, 0x01])
        self.write_enable()
        self.wr(0x0D, [0x00]); self.wr(0x0C, [0x01]); self.wr(0x0C, [0x00]); self.wr(0x0C, [0x00, 0x01])

    def write_fifo(self, addr, data):
        self.wr(0x00B3, [0x80])
        self.wr(addr, data)
        self.wr(0x00B3, [0x00])

    def sector_erase(self, addr):
        self.write_enable()
        self.wr(0x0D, [0x00]); self.wr(0x0C, [0x20])
        self.wr(0x0C, [(addr >> 16) & 0xFF]); self.wr(0x0C, [(addr >> 8) & 0xFF])
        self.wr(0x0C, [addr & 0xFF, 0x01])
        self.flush(); time.sleep(0.3)

    def write_blk(self, addr, data):
        self.write_enable()
        self.wr(0x0D, [0x00])
        self.write_fifo(0x0C, bytes([0x02, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF]) + data)
        self.wr(0x0D, [0x01])
        self.flush(); time.sleep(0.01)

    def soft_reset(self):
        self.wr(0x06F, [0x20]); self.flush()

    def activate(self, t):
        stop = sws_wr_addr(0x0602, b"\x05")  # CPU stop
        self.s.dtr = True; self.s.rts = True   # assert NRST
        time.sleep(0.1)
        self.s.dtr = False; self.s.rts = False
        self.soft_reset()
        t0 = time.time()
        while time.time() - t0 < t:
            self.s.write(stop)
        self.wr(0x00B2, [55])                # SWS speed
        self.s.write(stop)
        self.flash_byte_cmd(0xAB)            # flash wakeup
        self.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["write", "reset", "unlock", "erase"])
    ap.add_argument("file", nargs="?")
    ap.add_argument("-p", "--port", default="/dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=460800)
    ap.add_argument("-a", "--atime", type=float, default=3.0)
    a = ap.parse_args()

    sw = Swire(a.port, a.baud)
    if a.cmd == "reset":
        sw.activate(a.atime); sw.soft_reset(); print("reset"); return
    if a.cmd == "unlock":
        sw.activate(a.atime); sw.write_enable(); sw.unlock(); sw.flush(); time.sleep(3.5); print("unlocked"); return
    if a.cmd == "erase":
        sw.activate(a.atime); sw.write_enable(); sw.flash_byte_cmd(0x60); sw.flush(); time.sleep(3.5); print("erased"); return

    fw = open(a.file, "rb").read()
    if len(fw) < 16 or fw[8:12] != b"KNLT":
        sys.exit("not a telink firmware .bin")
    t0 = time.time()
    print("activating...")
    sw.activate(a.atime)
    sw.write_enable(); sw.unlock(); sw.flush(); time.sleep(1.5)
    addr = 0
    while addr < len(fw):
        if addr & 0xFFF == 0:
            sw.sector_erase(addr)
        blk = fw[addr:addr + 256]
        sw.write_blk(addr, blk)
        addr += len(blk)
        print(f"\r{addr * 100 // len(fw):3d}% 0x{addr:06x}", end="", flush=True)
    print(f"\ndone in {time.time() - t0:.1f}s, resetting")
    sw.soft_reset()


if __name__ == "__main__":
    main()
