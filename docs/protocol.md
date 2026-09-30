# ESP32 ↔ tag serial protocol

115200 8N1 between ESP32 GPIO43/44 and the tag's RXD (PA0) / TXD (PB1).
(The tag also accepts the same frames on its SWS pad via a software UART, which is how the
optional CH340 tools in `tools/` talk to it.)

## Frames (host → tag)

```
A5 5A | cmd (1) | len (u16 LE) | payload (len) | crc16-ccitt (u16 LE, init 0xFFFF, over cmd..payload)
```

Replies are text lines: `OK <cmd> ...` or `ER <reason>`.

| cmd | payload | reply | meaning |
|---|---|---|---|
| `P` | – | `OK P BWR213 v10 bank=0 n=3` | ping: panel type, firmware version, running flash bank, refreshes since boot (a drop means the tag rebooted and lost its framebuffer) |
| `F` | plane, value | `OK F` | fill a plane |
| `W` | plane, offset u16, data ≤ 597 B | `OK W` | write into a plane |
| `D` | [mode [red frames [led u16]]] | `OK D <temp>` | refresh: mode 0 = full 3-colour (~13 s), 1 = fast black/white (~1.6 s) with `red` as the LUT phase byte for red pixels; `led` = green LED brightness during the refresh in 0.1 % units |
| `L` | level u16, ms u16, [colour] | `OK L` | show the green (0) or red (1) LED at level (0.1 %) for ms |
| `O` | size u32, crc32 u32 | `OK O <bank addr>` | start a firmware update: erases the idle flash bank |
| `B` | offset u32, data ≤ 256 B | `OK B` | firmware block (256-byte pages) |
| `E` | – | `OK E` | verify CRC32, switch boot bank, reboot |

## Framebuffer layout

Two planes of 4000 bytes: 250 rows × 16 bytes (122 used pixels + padding), the panel's native
orientation. For a landscape pixel (x, y), x ∈ [0, 250), y ∈ [0, 122):

```
index = (y >> 3) + (249 - x) * 16      mask = 0x80 >> (y & 7)
black plane: bit 0 = black, 1 = white
red plane:   bit 1 = red
```

## Firmware update (dual bank)

The TLSR825x boot ROM starts the image at 0x00000 or 0x20000 whose byte 8 is `0x4B` ("K" of
the `KNLT` signature), checking 0x00000 first. `O`/`B`/`E` write the new image into the idle
bank with byte 8 blanked, verify the CRC32 of the whole image, then set the new bank's flag and
clear the old one. Until that last write the old firmware stays bootable.

## SWS recovery

With NRST and SWS wired, the ESP32 can write flash directly through the TLSR's single-wire debug
interface (UART at 460800 baud, one UART byte per SWire bit — the pvvx/atc1441 trick). The tag
firmware keeps SWS attached for 1.5 s after reset, which is the window the ESP32 uses to halt the
CPU. This is how a factory tag gets its first firmware.
