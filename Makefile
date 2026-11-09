CC      ?= clang
PREFIX  ?= /usr/local

# Universal 2 (Intel + Apple silicon). 10.9 is the oldest macOS current
# toolchains can still target (libarclite is gone from the SDK, so the ARC file is
# compiled with -fobjc-arc but linked without it; libobjc has the ARC calls since 10.7); the arm64 slice is always 11.0 or later.
ARCHS        ?= x86_64 arm64
MACOSX_MIN   ?= 10.9
ARCHFLAGS     = $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOSX_MIN)

V        = vendor/libusb
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -Isrc $(ARCHFLAGS)
# libusb is vendored and linked statically so the binaries and the app need nothing installed
LUFLAGS  = -O2 -std=gnu11 -I$(V) -I$(V)/config/macos $(ARCHFLAGS) -w
LUSRC    = $(V)/core.c $(V)/descriptor.c $(V)/hotplug.c $(V)/io.c $(V)/strerror.c $(V)/sync.c \
           $(V)/os/events_posix.c $(V)/os/threads_posix.c $(V)/os/darwin_usb.c
SYSLIBS  = -framework IOKit -framework CoreFoundation -framework Security \
           -framework CoreAudio -framework AudioToolbox

BUILD   = build
LIBUSB  = $(BUILD)/libusb-1.0.a
APP     = $(BUILD)/DazzleRecorder.app
# A small LGPL ffmpeg (scripts/build-ffmpeg.sh) goes into the app and next to the
# command line tool, so recording needs nothing installed. BUNDLE_FFMPEG=0 skips it.
BUNDLE_FFMPEG ?= 1
FFMPEG   = $(BUILD)/ffmpeg

all: $(BUILD)/dvc100-capture app $(if $(filter 1,$(BUNDLE_FFMPEG)),$(FFMPEG))

$(FFMPEG): scripts/build-ffmpeg.sh
	for a in $(ARCHS); do scripts/build-ffmpeg.sh macos-$$a $(BUILD)/ffmpeg-build/out/ffmpeg-$$a || exit 1; done
	lipo -create $(foreach a,$(ARCHS),$(BUILD)/ffmpeg-build/out/ffmpeg-$(a)) -output $@
	codesign --force --sign - $@

ffmpeg: $(FFMPEG)

$(BUILD):
	mkdir -p $(BUILD)/libusb

$(LIBUSB): $(LUSRC) $(V)/config/macos/config.h | $(BUILD)
	cd $(BUILD)/libusb && for f in $(LUSRC); do \
	    $(CC) $(LUFLAGS) -I$(CURDIR)/$(V) -I$(CURDIR)/$(V)/config/macos -c $(CURDIR)/$$f || exit 1; done
	rm -f $@ && ar rcs $@ $(BUILD)/libusb/*.o

$(BUILD)/dvc100.o: src/dvc100.c src/dvc100.h src/sysaudio.h src/em2828x_tables.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(V) -c $< -o $@

$(BUILD)/sysaudio.o: src/sysaudio.c src/sysaudio.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/audio_coreaudio.o: src/audio_coreaudio.c src/sysaudio.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/recorder.o: src/recorder.c src/recorder.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dvc100-capture: src/capture.c $(BUILD)/dvc100.o $(BUILD)/sysaudio.o $(BUILD)/audio_coreaudio.o $(BUILD)/recorder.o $(LIBUSB)
	$(CC) $(CFLAGS) $^ $(SYSLIBS) -o $@

$(BUILD)/dazzle-capture-app: src/viewer.m $(BUILD)/dvc100.o $(BUILD)/sysaudio.o $(BUILD)/audio_coreaudio.o $(BUILD)/recorder.o $(LIBUSB)
	$(CC) $(CFLAGS) -fobjc-arc -c src/viewer.m -o $(BUILD)/viewer.o
	$(CC) $(CFLAGS) $(BUILD)/viewer.o $(filter-out src/viewer.m,$^) $(SYSLIBS) \
	    -framework Cocoa -framework AVFoundation -framework CoreMedia -framework CoreVideo \
	    -framework CoreImage -framework QuartzCore -framework AudioUnit -o $@
	# Newer SDKs' CoreMedia.tbd has a $$ld$$previous rule that, for deployment targets
	# 10.9-10.14.4, records CoreMedia as @rpath/libswiftCoreMedia.dylib, which does not
	# exist there. The C symbols have always lived in CoreMedia itself, so point it back.
	if otool -L $@ | grep -q '@rpath/libswiftCoreMedia.dylib'; then \
	    install_name_tool -change @rpath/libswiftCoreMedia.dylib \
	        /System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia $@ 2>/dev/null && \
	    codesign --force --sign - $@; fi

app: $(BUILD)/dazzle-capture-app resources/Info.plist resources/AppIcon.icns $(if $(filter 1,$(BUNDLE_FFMPEG)),$(FFMPEG))
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	sed 's/@MACOSX_MIN@/$(MACOSX_MIN)/' resources/Info.plist > "$(APP)/Contents/Info.plist"
	cp resources/AppIcon.icns "$(APP)/Contents/Resources/AppIcon.icns"
	cp $(BUILD)/dazzle-capture-app "$(APP)/Contents/MacOS/DazzleRecorder"
	if [ "$(BUNDLE_FFMPEG)" = 1 ]; then \
	    cp $(FFMPEG) "$(APP)/Contents/MacOS/ffmpeg" && \
	    mkdir -p "$(APP)/Contents/Resources/licenses" && cp resources/licenses/* "$(APP)/Contents/Resources/licenses/"; fi
	codesign --force --sign - "$(APP)"

$(BUILD)/test_assembler: tests/test_assembler.c src/dvc100.c src/sysaudio.c src/audio_coreaudio.c $(LIBUSB)
	$(CC) $(CFLAGS) -I$(V) $^ $(SYSLIBS) -o $@

test: $(BUILD)/test_assembler
	$(BUILD)/test_assembler

install: all
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/dvc100-capture $(PREFIX)/bin/
	cp -R "$(APP)" /Applications/

clean:
	rm -rf $(BUILD)

.PHONY: all app ffmpeg test install clean
