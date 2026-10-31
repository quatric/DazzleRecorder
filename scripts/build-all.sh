#!/bin/sh
# Cross-builds the Dazzle Capture command line tool (and the Windows GUI) for
# Windows and Linux with Zig's bundled clang. Nothing else is needed: libusb
# is vendored and ALSA is loaded at run time.
#   ZIG=/path/to/zig scripts/build-all.sh [target ...]
# Targets are Zig triples; the default is the full list below.
set -e
cd "$(dirname "$0")/.."
ZIG=${ZIG:-zig}
OUT=${OUT:-dist}
CACHE=${ZIG_CACHE:-$PWD/build/zig-cache}
export ZIG_GLOBAL_CACHE_DIR=$CACHE ZIG_LOCAL_CACHE_DIR=$CACHE

TARGETS=${*:-"x86_64-windows-gnu x86-windows-gnu aarch64-windows-gnu \
x86_64-linux-gnu.2.31 x86-linux-gnu.2.31 aarch64-linux-gnu.2.31 arm-linux-gnueabihf.2.31 \
x86_64-linux-musl aarch64-linux-musl"}

V=vendor/libusb
COMMON="-O2 -Wall -Wextra -std=gnu11 -Isrc -I$V"
SRC="src/dvc100.c src/sysaudio.c src/recorder.c src/app_core.c"

for t in $TARGETS; do
  name=$(echo "$t" | sed 's/\.2\.31$//')
  b=build/$name; mkdir -p $b "$OUT/$name"
  case $t in
  *windows*)
    os=windows; ext=.exe
    LU="$V/core.c $V/descriptor.c $V/hotplug.c $V/io.c $V/strerror.c $V/sync.c $V/os/events_windows.c \
$V/os/threads_windows.c $V/os/windows_common.c $V/os/windows_usbdk.c $V/os/windows_winusb.c"
    FL="-I$V/config/windows "
    OSSRC="src/audio_windows.c"
    LIBS="-lole32 -loleaut32 -lwinmm -luuid -lsetupapi -ladvapi32"
    GUI="src/gui_win32.c"; GUILIBS="-lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -ldwmapi -Wl,--subsystem,windows"
    ;;
  *linux*)
    os=linux; ext=
    LU="$V/core.c $V/descriptor.c $V/hotplug.c $V/io.c $V/strerror.c $V/sync.c $V/os/events_posix.c \
$V/os/threads_posix.c $V/os/linux_usbfs.c $V/os/linux_netlink.c"
    FL="-I$V/config/linux"
    OSSRC="src/audio_alsa.c"
    LIBS="-lpthread -lm -ldl"
    GUI="src/gui_x11.c"; GUILIBS="-lpthread -lm -ldl"
    ;;
  *) echo "unsupported target $t" >&2; exit 1 ;;
  esac
  echo "== $name"
  $ZIG cc -target $t $COMMON $FL -DDAZZLE_VENDORED_LIBUSB $LU $SRC $OSSRC src/capture.c $LIBS \
      -o "$OUT/$name/dvc100-capture$ext"
  if [ -f "$GUI" ]; then
    $ZIG cc -target $t $COMMON $FL $LU $SRC $OSSRC $GUI $GUILIBS $LIBS \
        -o "$OUT/$name/Dazzle Capture$ext"
  fi
  rm -f "$OUT/$name"/*.pdb
done
