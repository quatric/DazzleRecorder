# Reverse engineering notes: Dazzle DVC100 Windows driver

Source package: `Dazzle Driver DVC.zip` → `Data1.cab`
(Dazzle Video Capture DVC100 x64 driver 1.09).

| File | Role |
|---|---|
| `emdevice64.sys` | eMPIA "USB 28xx WDM Driver" 2.2020.1022.0, stream-class video minidriver. PDB path `D:\__eMPIA_Wrok\EM2820\x64\Release\emDevice64.pdb` |
| `emaudio64.sys` | Audio driver for interface `MI_01`. It only sends USB Audio Class requests plus AC97 register pokes |
| `emvideo.inf` | Binds `USB\VID_2304&PID_021A&MI_00` and holds the registry defaults |
| `emprop*.ax`, `emtwain*.ds`, `emusd64.dll`, `emvfw.dll`, `emyuv.dll` | DirectShow property pages, TWAIN/STI, VfW: not needed |

Both `.sys` files were decompiled with Ghidra 12.1.2 in headless mode
(842 and 135 functions). The addresses below are from `emdevice64.sys`.

## Hardware

* Bridge: eMPIA em28xx family, identified by reg `0x0a`. The tested DVC100 reports `0x12` (em2820).
* Video decoder: Philips SAA7113 at 8-bit I2C address `0x4a`. The driver probes
  `0x42`, `0x4a`, `0x48`, `0xb8` and tags the SAA7113 as decoder type `0x134a` (`0x140005488`).
* Audio: an AC97 codec on the em28xx, exposed as USB Audio Class 1.0: interface 1 (AudioControl:
  line-in terminal → feature unit 2 with mute/volume → USB streaming terminal) and interface 2
  (AudioStreaming, alt 1–5 = 48000/44100/32000/22050/8000 Hz, 16-bit stereo, iso endpoint `0x83`,
  `bInterval` 1 = every microframe). AppleUSBAudio creates an `AppleUSBAudioControlNub` for
  interface 1 but no audio device, so the port reads interface 2 itself.

## USB transport

All register traffic goes through one helper that builds a `URB_FUNCTION_VENDOR_DEVICE`
(0x17) control request, `0x140012e70`:

| Operation | bmRequestType | bRequest | wIndex | Notes |
|---|---|---|---|---|
| reg read | `0xC0` | 0 | register | `0x140011ff0` (8-bit), `0x140012070` (16), `0x140012030` (32) |
| reg write | `0x40` | 0 | register | `0x140012140`, `0x140012290`, `0x1400121b0` |
| I2C write | `0x40` | 2 (stop) / 3 (no stop) | 8-bit address | then poll reg `0x05`: `0` done, bit `0x10` = NACK (`0x1400122fc`) |
| I2C read | `0xC0` | 2 | 8-bit address | same status check |
| AC97 write | reg `0x40` ← value (LE16), reg `0x42` ← AC97 reg | | | wait for reg `0x43` bit 0 clear first (`0x14000d7dc`) |

Video streams on interface 0, isochronous endpoint `0x82`. The driver tries alternate
settings from the highest down (`0x14000bae4`, `0x140009f70`); the port picks the
one with the largest `wMaxPacketSize`.

## Bring-up sequence

1. reg `0x06` ← `0x40` (I2C clock, wait enable) (`0x14000d5c0`)
2. GPIO reg `0x08` bit 3 ← 1, wait 100 ms: decoder power (`0x14000e1bc`)
3. SAA7113 (`0x1400063b8`, `0x140006478`, `0x1400062c8`):
   * `01`=08, `03`=30, `06..09` = EB 0D 88 01 (`09` = 81 for S-Video), `0A..0D` = 80 47 40 00, `0F`=2A, `10..13` = 08 0C E7 00
   * standard: `0A`=80, `0E`=01, `08`=88 (automatic 50/60 Hz field detection)
   * input: `02` = `0xC0 | mode`, registry `Mode7113` = `0x090002`, so composite = AI11 (mode 0), S-Video = mode 9
   * status: reg `1F` bit 0 = locked, bit 5 = 60 Hz (`0x140005f6c`)
4. em28xx capture window (`0x14000c910`):
   * `15/16/17` = 20, `18/19/1A` = 0, `23/24/26` = 0, `13` = 08, `12` = 27 (mask AF)
   * `27` output format = `0x34` (16 bpp YUV 4:2:2, Y0 Cb Y1 Cr, interlaced) (`0x14000d2c0`)
   * `11` = 11 (CCIR656, interlaced) (mask 17)
   * accumulator `28..2B` = 1, w/4-1, 1, h/4-1
   * capture area `1C..1F` = hstart, vstart, w/4, h/4, overflow bits in `1B`.
     With vstart 0 the first two lines of the odd field carry VBI (closed caption) data;
     the port uses vstart 2, like Linux em28xx.
   * scaler `30..33` with enable bits in `26` (`0x14000d68c`)
5. AC97 (`0x14000d894`, `0x14000c360`): `26`=4200, `1A`=0505, `2A`=0031, `32`=48000,
   aux/mic/video muted, the selected input unmuted. The tested unit's codec is a SigmaTel
   STAC97xx (vendor ID `8384:7650`); recording its stereo mix (`1A`=0505) dropped the left
   channel, so the port records line-in directly (`1A`=0404), as Linux em28xx does. Audio source is reg `0x0e` bits 6–7
   (line = 2), then reg `0x0f` bit 7 unmutes.
6. Start (`0x14000e26c`): reg `0x0c` |= `0x10`, reg `0x12` |= `0x40`.
   Stop (`0x14000e234`): reg `0x12` &= ~`0x40`, wait 33 ms, reg `0x0c` &= ~`0x10`.

## Isochronous payload (`0x14000acdc`)

* `22 5A ss xx`: start of a field. Bit 0 of `ss` set = odd (bottom) field. 4-byte header.
* `33 95 ..`: VBI block (VBI capture is off in this port).
* `88 88 88 88`: per-packet prefix on newer chips, stripped.
* Anything else continues the current field. Fields are 720 × (480|576)/2 lines of YUYV, woven into frames.

# Black Dazzle (USB `1B80:E60A`, chip ID `0x92`)

Two Windows drivers bind this box: `embda64.sys` from `Dazzle Driver BDA.zip` (the
default, shows it as "Roxio Video Capture USB") and `emDevice64.sys` from the DVC100
package, installed by hand. The working Mac path follows **emDevice64**. embda64 looks
for an I2C decoder this board does not have.

## Bridge

* Chip ID `0x92` (em2828x, chip class 6 in both drivers).
* I2C goes through vendor requests. The EEPROM (`0xa0`) is on the first bus (reg `0x06` =
  `0x40`); `0x44` selects a second bus. On this board nothing is wired to the second bus:
  its lines read low, and with clock-wait (`0x44`) any transfer hangs the bridge until
  it is replugged. Reg `0x93` bit 5 (port 3 pin 5 pull-up) lets it idle high, but no
  device answers there.
* Regs `0x90`–`0x97` are pin configuration; `0x84`–`0x87` read the pin levels. Clearing
  reg `0x83` bit 6 drives a first-bus line low and hangs that bus.
* Audio is a normal USB Audio Class 1 interface with 1 ms packets, so macOS publishes
  "Dazzle Video Capture USB Audio Device" and AppleUSBAudio keeps interface 2. The
  port records that device through Core Audio (`src/coreaudio_in.c`).

## EEPROM

16-bit addressed, at I2C `0xa0`. Byte `0x0001` = page; the 16-bit little-endian word at
`page:0x32`, plus `page*256 + 4`, is the start of the board config (`0x64` on this box),
which starts with the signature `1A EB 67 95`.

| Config byte | Value here | Meaning |
|---|---|---|
| `+0x08` (word) | `0x1350` | bits 14–15 pick the second-bus speed/mode |
| `+0x20` | `0x02` | bit 1 set: run decoder detection (clear: embda64 writes regs `0xb1`/`0xb3`/`0xb5` = 0, `0xb6` = `0x0f`) |
| `+0x28` | `0x10` | "VideoDecoderI2C": `0x10` = decoder built into the bridge (emDevice64 type `0x104a`) |
| `+0x4c..0x5f` | | board GPIO map (only an LED on this box) |

## Decoder: built into the bridge, registers `0x7a00`–`0x7aff`

Plain register reads/writes (vendor request 0) with 16-bit `wIndex`. From emDevice64:

| Step | Function | What it does |
|---|---|---|
| base | `0x1400085d4` | reg `0x1c` = 2, `0x1d` = 0; `0x7a0b`, `0x7a1c`–`0x7a1f`, `0x7a2f`, `0x7a40`; pulse `0x7a3f` |
| standard | `0x140008690` | per standard (Windows 0x1 = NTSC-M, 0x10 = PAL-B) and input: `0x7a00`–`0x7a0f`, `0x7a18`–`0x7a1b`, `0x7a2e`, `0x7a30`, `0x7a80`–`0x7a82`, `0x7ac0`/`0x7ac1`, comb/AGC regs `0x7a28`–`0x7a67`, then regs `0xb0`/`0xb2`; pulse `0x7a3f` to latch |
| input | `0x140008508` | regs `0x38`, `0x2e`, `0xb8`, `0xb6` = `0x8f`, `0x7a29`; Windows input 2 = composite, 3 = S-Video (input 1 gives the blue no-signal screen on this box); both verified on hardware, NTSC |
| status | `0x14000833c` | reads `0x7a65` and `0x7a69` / `0x7a6a`, then writes back `0x7a65 & 0x20` |

On the hardware `0x7a69`/`0x7a6a` stay 0 with a picture present; `0x7a6c` bit 7 and
`0x7a6e` bit 7 (and `0x7a65` bit 1, `0x7a6f` bit 0) are set only while a signal is present,
so the port uses those for lock.

Brightness, contrast and saturation use the bridge's colour stage (regs `0x20`–`0x22`).
The capture window, output format and streaming are the same as the DVC100.

## embda64 path (other em2828x boards)

embda64 asks the OEM filter `emoem64.sys` first (`0x14004911c` probes I2C `0x88`, a
Conexant-style "Merlin" decoder loaded with `merlinFW.rom`), then probes `0x42`/`0x4a`/
`0x48`/`0xb8`/`0x82` on the second bus. The library keeps the `0x82` decoder code
(type `0xC1B82`, register tables in `src/em2828x_tables.h` from
`tools/gen_em2828x_tables.py`) for boards whose config byte `+0x28` is not `0x10`. That
path is untested: no such board was available.
