CC      ?= clang
PREFIX  ?= /usr/local
LIBUSB_PREFIX ?= $(shell brew --prefix libusb 2>/dev/null || echo /opt/homebrew)

CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -I$(LIBUSB_PREFIX)/include/libusb-1.0 -Isrc
# libusb is linked statically so the binaries and the app need nothing installed
LIBUSB   = $(LIBUSB_PREFIX)/lib/libusb-1.0.a
SYSLIBS  = -framework IOKit -framework CoreFoundation -framework Security \
           -framework CoreAudio -framework AudioToolbox

BUILD   = build
APP     = $(BUILD)/Dazzle Capture.app

all: $(BUILD)/dvc100-capture app

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/dvc100.o: src/dvc100.c src/dvc100.h src/sysaudio.h src/em2828x_tables.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/sysaudio.o: src/sysaudio.c src/sysaudio.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/audio_coreaudio.o: src/audio_coreaudio.c src/sysaudio.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/recorder.o: src/recorder.c src/recorder.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dvc100-capture: src/capture.c $(BUILD)/dvc100.o $(BUILD)/sysaudio.o $(BUILD)/audio_coreaudio.o $(BUILD)/recorder.o
	$(CC) $(CFLAGS) $^ $(LIBUSB) $(SYSLIBS) -o $@

$(BUILD)/dazzle-capture-app: src/viewer.m $(BUILD)/dvc100.o $(BUILD)/sysaudio.o $(BUILD)/audio_coreaudio.o $(BUILD)/recorder.o
	$(CC) $(CFLAGS) -fobjc-arc $^ $(LIBUSB) $(SYSLIBS) \
	    -framework Cocoa -framework AVFoundation -framework CoreMedia -framework CoreVideo \
	    -framework CoreImage -framework QuartzCore -o $@

app: $(BUILD)/dazzle-capture-app resources/Info.plist resources/AppIcon.icns
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp resources/Info.plist "$(APP)/Contents/Info.plist"
	cp resources/AppIcon.icns "$(APP)/Contents/Resources/AppIcon.icns"
	cp $(BUILD)/dazzle-capture-app "$(APP)/Contents/MacOS/Dazzle Capture"
	codesign --force --sign - "$(APP)"

$(BUILD)/test_assembler: tests/test_assembler.c src/dvc100.c src/sysaudio.c src/audio_coreaudio.c
	$(CC) $(CFLAGS) $^ $(LIBUSB) $(SYSLIBS) -o $@

test: $(BUILD)/test_assembler
	$(BUILD)/test_assembler

install: all
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/dvc100-capture $(PREFIX)/bin/
	cp -R "$(APP)" /Applications/

clean:
	rm -rf $(BUILD)

.PHONY: all app test install clean
