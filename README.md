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
[Releases](https://github.com/quatric/DazzleRecorder/releases) page. Each archive contains the command line
tool `dvc100-capture` and the GUI `Dazzle Capture` (a live preview with sound, input selection and recording;
see [Dazzle Capture (app)](#dazzle-capture-app)).

### macOS

Download `DazzleRecorder-macos-arm64-app.zip` (Apple Silicon), unzip it and open **Dazzle Capture**. The app
is ad-hoc signed, not notarized, so the first time right-click it and choose **Open** (or allow it under
System Settings → Privacy & Security). It may ask for microphone access on the black Dazzle. Recording needs
`brew install ffmpeg`. `DazzleRecorder-macos-arm64.tar.gz` also contains the command line tool.

### Windows

1. Download the zip for your machine: `x86_64-windows-gnu` for most PCs, `x86-windows-gnu` for 32-bit,
   `aarch64-windows-gnu` for Arm.
2. Install the WinUSB driver for the Dazzle with [Zadig](https://zadig.akeo.ie): plug the device in, pick it in
   the list (options → List All Devices), choose **WinUSB** and click Install Driver. This replaces the vendor
   driver for that device.
3. Install [ffmpeg](https://ffmpeg.org/download.html) and put it on your `PATH` for recording.
4. Run `Dazzle Capture.exe`. Windows SmartScreen may warn about an unsigned app: choose More info → Run anyway.

### Linux

1. Download the tarball for your machine (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `x86-linux-gnu` or
   `arm-linux-gnueabihf`; the `musl` builds are for distributions without glibc, such as Alpine) and extract it.
2. Allow access to the USB device with a udev rule, then replug the device:

   ```bash
   echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2304", MODE="0666"
   SUBSYSTEM=="usb", ATTR{idVendor}=="1b80", MODE="0666"' | sudo tee /etc/udev/rules.d/99-dazzle.rules
   sudo udevadm control --reload-rules
   ```

3. Install `ffmpeg` for recording (and ALSA for sound, which is present on most desktops).
4. Run `./"Dazzle Capture"`. The GUI uses X11 and also runs under XWayland.

## Build (macOS)

```bash
brew install libusb
brew install ffmpeg   # used for MPEG-2 encoding
make            # build/dvc100-capture and "build/Dazzle Capture.app"
make test       # packet/field assembler unit test
```

## Use

### Dazzle Capture (app)

On macOS run `open "build/Dazzle Capture.app"`; on Windows and Linux run the `Dazzle Capture` executable.

The GUI (Cocoa on macOS, Win32 on Windows, Xlib on Linux) shows a 4:3 live preview with sound, and a control bar for:

* **Input**: Composite or S-Video
* **Deinterlace**: applies to the preview and the recording. Off records interlaced
  MPEG-2 (top field first) like a DVD, which is best for burning a DVD later. On
  uses ffmpeg's yadif and suits computer playback and editing.
* **Quality**: 6, 8 or 9.5 Mbit/s MPEG-2 video (DVD-legal), 224 kbit/s MP2 stereo audio
* **Save to**: recording folder (default: your Movies or Videos folder)
* **Record / Stop** (⌘R on macOS): writes `Dazzle Capture <date>.mpg`, a DVD-compatible
  MPEG-2 program stream with a 4:3 display aspect
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

Options: `-i composite|svideo`, `-n auto|ntsc|pal`, `-f y4m|yuyv`, `-o FILE`,
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

`scripts/build-all.sh` cross-builds from any machine with [Zig](https://ziglang.org) (0.17 or newer) and needs
nothing else; libusb 1.0.30 is vendored in `vendor/libusb` (LGPL-2.1) and ALSA and X11 are loaded at run time.

```sh
ZIG=/path/to/zig scripts/build-all.sh                  # everything into dist/<target>/
ZIG=/path/to/zig scripts/build-all.sh aarch64-linux-gnu.2.31   # one target
```

Targets: Windows x86_64, x86 and arm64; Linux x86_64, x86, arm64 and armhf (glibc 2.31) plus x86_64 and arm64
(musl). Each folder has `dvc100-capture` (command line) and `Dazzle Capture` (GUI: Win32 on Windows, Xlib on
Linux, which also runs under XWayland). On Linux, grant access to the USB device with a udev rule, for example
`SUBSYSTEM=="usb", ATTR{idVendor}=="1b80", MODE="0666"`, and install `ffmpeg` for recording. The Windows build
uses WinUSB, so the box needs the WinUSB driver (Zadig) instead of the vendor driver.
