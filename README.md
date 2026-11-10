# DazzleRecorder

A userspace capture driver and recorder for the Dazzle DVC100 (USB `2304:021a`, also the DVC90 `2304:0207`)
and the black Dazzle Video Capture USB (`1B80:E60A`, also sold as Roxio Video Capture USB), for
macOS, Windows and Linux. It needs **no kernel extension, system extension or vendor driver**. It talks to the
em28xx bridge through libusb.

* **Video**: interface 0, isochronous endpoint 0x82, 720×480 (NTSC) or 720×576 (PAL)
  YUYV at full frame rate.
* **Audio**: the audio interface is USB Audio Class 1.0, but on a high-speed bus
  with one packet per 125 µs microframe. Operating system audio drivers (for example macOS's AppleUSBAudio)
  attach to it and may never publish a usable audio device, so this code reads the streaming interface
  (interface 2, endpoint 0x83, 48 kHz 16-bit stereo) over libusb as well.

Tested on a DVC100 (em2820 bridge, chip ID 0x12; SAA7113 decoder, version 0x11),
NTSC composite input from a DVD player, and on a black Dazzle (em2828x bridge, chip ID
0x92, decoder built into the bridge), NTSC composite and S-Video. On the black Dazzle, sound comes
from the USB audio device the operating system already shows, so the app may ask for microphone access.

The register sequences come from decompiling the vendor drivers `emDevice64.sys` and
`emBDA64.sys`;
see [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

## Download

Prebuilt macOS, Windows and Linux builds are on the
[Releases](https://github.com/quatric/DazzleRecorder/releases) page. Each one contains the command line
tool `dvc100-capture`, the GUI `DazzleRecorder` (a live preview with sound, input selection and recording;
see [DazzleRecorder (app)](#dazzlerecorder-app)) and a small `ffmpeg` for recording, so nothing else needs
to be installed.

### macOS

Download `DazzleRecorder-macos-universal-app.zip` (Universal 2: Apple silicon and Intel, macOS 10.9 Mavericks
or later), unzip it and open **DazzleRecorder**. The app
is ad-hoc signed, not notarized, so the first time right-click it and choose **Open** (or allow it under
System Settings → Privacy & Security). It may ask for microphone access on the black Dazzle.
`DazzleRecorder-macos-universal.tar.gz` also contains the command line tool and its `ffmpeg`.

#### Older Macs (PowerPC, Mac OS X 10.3 to 10.8)

`DazzleRecorder-macos-legacy-ppc-i386-app.zip` holds **DazzleRecorder Legacy**, the same app for older
Macs: PowerPC (G3, G4, G5) on Mac OS X 10.3.9 or later, and Intel on 10.4 or later. Its preview uses OpenGL
and its sound the old Core Audio interfaces. A PowerPC Mac cannot encode H.264 in real time, so there it records
**DV** (the default, light on the CPU) or **MPEG-2** (with a fast setting; a G4 or G5 is needed); on Intel it
records MPEG-2 (the default), H.264 or DV. `DazzleRecorder-macos-legacy-ppc-i386.tar.gz` adds the command line
tool. These builds have not been tested on real old hardware yet; reports are welcome.

### Windows

Windows 7 SP1 and later, 32- and 64-bit, and Windows on Arm.

1. Download the installer for your machine: `DazzleRecorder-x86_64-windows-setup.exe` for most PCs,
   `x86` for 32-bit Windows, `aarch64` for Windows on Arm.
2. Run it and keep **Dazzle driver** ticked. With the Dazzle plugged in, it puts a driver on the Dazzle's
   video interface the way [Zadig](https://zadig.akeo.ie) does: WinUSB on Windows 8.1 and later, and the
   signed libusbK driver on Windows 7 and 8.0, whose WinUSB cannot stream video. The sound interface keeps
   Windows' own audio driver. Windows SmartScreen may warn about an unsigned installer: choose More info →
   Run anyway.
3. Start **DazzleRecorder** from the Start menu. If the Dazzle was not plugged in during setup, or it still
   has the manufacturer's driver, the app offers to install the driver when it starts (this needs
   administrator rights).

The driver tool can also be run by hand from the install folder: `dazzle-driver install` (`--winusb` or
`--libusbk` to choose), `dazzle-driver status`, and `dazzle-driver uninstall`, which gives the device back to
the driver it had before. To sign its driver package, it makes a certificate on your PC, trusts it for code
signing, and deletes the certificate's private key straight after signing, so nothing else can be signed with
it. The `.zip` downloads hold the same files for running without an installer (run `dazzle-driver.exe` once).
If the [UsbDk](https://github.com/daynix/UsbDk/releases) driver is installed, the app can use it instead.

### Linux

1. Download the tarball for your machine (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `x86-linux-gnu` or
   `arm-linux-gnueabihf`; the `musl` builds are for distributions without glibc, such as Alpine) and extract it.
2. Allow access to the USB device with a udev rule, then replug the device:

   ```bash
   echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2304", MODE="0666"
   SUBSYSTEM=="usb", ATTR{idVendor}=="1b80", MODE="0666"' | sudo tee /etc/udev/rules.d/99-dazzle.rules
   sudo udevadm control --reload-rules
   ```

3. ALSA is needed for sound (present on most desktops). The bundled static `ffmpeg` is used for recording.
4. Run `./DazzleRecorder`. The GUI uses X11 and also runs under XWayland.

## Build (macOS)

```bash
brew install nasm     # for ffmpeg's x86 assembly
make            # build/dvc100-capture, build/ffmpeg and "build/DazzleRecorder.app" (Universal 2, macOS 10.9+)
make test       # packet/field assembler unit test
```

libusb is built from `vendor/libusb`. `make` also builds a small ffmpeg 7.1 with x264 from source
(`scripts/build-ffmpeg.sh`, a minute or two) and puts it inside the app; `make BUNDLE_FFMPEG=0` skips that and
uses an ffmpeg on your `PATH` instead. `make ARCHS=arm64 MACOSX_MIN=11.0` builds a single-architecture binary.

The legacy app is built by `scripts/build-legacy-mac.sh` in the `powerpc-apple-darwin8` cross toolchain
(`ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u`, with clang, nasm and
pkg-config added), as the release workflow does. It checks that the PowerPC slice only uses what Mac OS X
10.3.9 has.

## Use

### DazzleRecorder (app)

On macOS run `open "build/DazzleRecorder.app"`; on Windows and Linux run the `DazzleRecorder` executable.

The GUI (Cocoa on macOS, Win32 on Windows, Xlib on Linux) shows a 4:3 live preview with sound, and a control bar for:

* **Input**: Composite or S-Video
* **Widescreen**: off by default. Stretches the picture to 16:9 in the preview, snapshots and recordings
  (the MPEG-2 file is flagged 16:9, so players stretch it too). Use it for tapes recorded in anamorphic widescreen.
* **Stop after**: stops the recording by itself after a time you choose (default: no limit). Pick 5 minutes up to
  3 hours, or type your own time into the box: `45`, `90 min`, `2h`, `1:30`. It can be changed during a recording.
  Linux: the "Stop:" button cycles through the choices, and `+` / `-` change the time by a minute.
* **Preview**: on by default. Turn it off to stop drawing the picture and save CPU on slow computers; recording and
  the sound carry on. Snapshots need it on (Linux: `E`; Mac: View > Show Preview, `Cmd+E`).
* **Deinterlace**: applies to the preview and the recording. Off records interlaced
  video (top field first) like a DVD, which is best for burning a DVD later. On
  uses ffmpeg's yadif and suits computer playback and editing.
* **Format**: **MPEG-2** (the default) writes a DVD-compatible `.mpg`; **H.264** writes a much smaller
  `.mp4` with x264 and AAC audio, better for keeping on a computer or sharing. On Windows and Linux both
  are in the Quality list.
* **Quality**: MPEG-2 at 6, 8 or 9.5 Mbit/s video (DVD-legal) with 224 kbit/s MP2 stereo audio, or H.264 at
  x264 constant quality 23, 20 or 17 (lower is better) with 192 kbit/s AAC
* **Save to**: recording folder (default: your Movies or Videos folder)
* **Record / Stop** (⌘R on macOS): writes `DazzleRecorder <date>.mpg` (MPEG-2 program stream) or `.mp4`
  (H.264), with a 4:3 display aspect (16:9 with Widescreen)
* **Audio** source (macOS) / **Line In (not Dazzle)** (Windows) / **Audio: Line In** (Linux): record and monitor the computer's Line In
  instead of the Dazzle's own audio, bypassing it (also `--line-in` on macOS)
* L/R audio meters, **Play audio** to monitor through the speakers, ⌘S saves a PNG snapshot

Audio is the master clock while recording. If video frames go missing (signal loss,
a stalled disk), frames are repeated so sound and picture stay in sync.

Print device information and signal status:

```bash
build/dvc100-capture --info
```

Watch in ffplay:

```bash
build/dvc100-capture | ffplay -vf yadif -
```

### Command line

Record 60 seconds to MPEG-2 (add `-d` to deinterlace, `-b 9500` for bitrate):

```bash
build/dvc100-capture -t 60 -m tape.mpg
```

Or to H.264 with x264 (`-q 18` for higher quality):

```bash
build/dvc100-capture -t 60 -x tape.mp4
```

Record 60 seconds of raw video and audio, then mux to MP4:

```bash
build/dvc100-capture -t 60 -o clip.y4m -a clip.wav
```

```bash
ffmpeg -i clip.y4m -i clip.wav -vf yadif -c:v libx264 -crf 18 -c:a aac clip.mp4
```

Audio only:

```bash
build/dvc100-capture --audio-only -a tape.wav
```

Options: `-x FILE.mp4` (H.264 instead of MPEG-2), `-q CRF` (H.264 quality, default 20), `--dv FILE.mov`
(DV, light on old CPUs), `-w` (flag the recording 16:9), `-i composite|svideo`, `-n auto|ntsc|pal`, `-f y4m|yuyv`, `-o FILE`,
`-c FRAMES`, `-t SECONDS`, `-m FILE.mpg`, `-d`, `-b KBPS`, `-a FILE.wav`, `--audio-only`, `--brightness/--contrast/--saturation/--hue 0..255`, `-v`/`-vv`
for register traces.

## Limits

* It does not show up as a system camera or audio input (FaceTime, Zoom, QuickTime, and so on). On macOS that
  would need a CoreMediaIO Camera Extension, which must be built with Xcode and signed with an
  Apple Developer team. The `dvc100` library in `src/` is written so that such an
  extension can reuse it.
* Output is the full 720×480 / 720×576 interlaced frame. Scaling and deinterlacing
  happen in the viewer or ffmpeg.

## Build (Windows and Linux)

`scripts/build-all.sh` cross-builds from macOS or Linux with [Zig](https://ziglang.org) (0.17 or newer) and
nasm; libusb 1.0.30 is vendored in `vendor/libusb` (LGPL-2.1) and ALSA and X11 are loaded at run time.

```sh
ZIG=/path/to/zig scripts/build-all.sh                  # everything into dist/<target>/
ZIG=/path/to/zig scripts/build-all.sh aarch64-linux-gnu.2.31   # one target
```

Targets: Windows x86_64, x86 and arm64; Linux x86_64, x86, arm64 and armhf (glibc 2.31) plus x86_64 and arm64
(musl). Each folder has `dvc100-capture` (command line), `DazzleRecorder` (GUI: Win32 on Windows, Xlib on
Linux, which also runs under XWayland) and a static `ffmpeg` (`BUNDLE_FFMPEG=0` skips it).

For Windows 7, the Windows builds must use an msvcrt-based MinGW toolchain, because Zig links the Universal
CRT, which Windows 7 does not have. The release builds use [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)'s
msvcrt variant:

```sh
LIBUSBK_DIR=$(scripts/fetch-libusbk.sh) WIN_CC_x86_64=x86_64-w64-mingw32-clang WIN_CC_x86=i686-w64-mingw32-clang \
  WIN_CC_aarch64=aarch64-w64-mingw32-clang scripts/build-all.sh x86_64-windows-gnu x86-windows-gnu aarch64-windows-gnu
makensis -DARCH=x86_64 -DVERSION=0.4.0 -DSRC=dist/x86_64-windows-gnu -DOUTFILE=setup.exe installer/windows.nsi
```

`scripts/fetch-libusbk.sh` downloads the signed libusbK 3.1 driver (BSD licence) that the Windows 7 builds
install. The bundled ffmpeg includes x264 for H.264 recording, which makes it GPL-2.0-or-later (`X264=0` builds an LGPL
ffmpeg without it); see `resources/licenses/`.

## Troubleshooting sound

`dvc100-capture --info -v` prints the AC97 codec's registers on the DVC100. The codec records its line input;
if a board is wired to another input, try `DVC100_AC97="1a=0505"` (stereo mix) or `1a=0101`/`0202`/`0303`
(CD, video, aux) in the environment, for example `DVC100_AC97="1a=0505" dvc100-capture --audio-only -a test.wav -t 5`.
