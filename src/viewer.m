/*
 * Dazzle Capture: live preview and MPEG-2 recording for the Dazzle DVC100.
 *
 * The control bar under the preview selects the input, deinterlacing,
 * recording quality and folder, and starts/stops recording to a
 * DVD-compatible MPEG-2 program stream (.mpg).
 */
#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <os/lock.h>

#include "dvc100.h"
#include "recorder.h"

/* ---- preview view: keeps the picture at 4:3 inside whatever space it has ---- */

@interface DVCPreview : NSView
@property(nonatomic, strong) AVSampleBufferDisplayLayer *displayLayer;
@property(nonatomic, strong) NSTextField *badge;
@end

@implementation DVCPreview
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = NSColor.blackColor.CGColor;
        _displayLayer = [AVSampleBufferDisplayLayer layer];
        /* 720 BT.601 samples span a 4:3 picture, so stretch into a 4:3 rect */
        _displayLayer.videoGravity = AVLayerVideoGravityResize;
        [self.layer addSublayer:_displayLayer];

        _badge = [NSTextField labelWithString:@""];
        _badge.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightSemibold];
        _badge.textColor = NSColor.whiteColor;
        _badge.backgroundColor = [NSColor colorWithRed:0.8 green:0.05 blue:0.05 alpha:0.85];
        _badge.drawsBackground = YES;
        _badge.hidden = YES;
        [self addSubview:_badge];
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (void)layout
{
    [super layout];
    NSSize s = self.bounds.size;
    CGFloat w = s.width, h = w * 3 / 4;
    if (h > s.height) {
        h = s.height;
        w = h * 4 / 3;
    }
    NSRect r = NSMakeRect((s.width - w) / 2, (s.height - h) / 2, w, h);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _displayLayer.frame = r;
    [CATransaction commit];
    [_badge sizeToFit];
    _badge.frame = NSMakeRect(NSMinX(r) + 10, NSMinY(r) + 10, _badge.frame.size.width + 8, 20);
}
@end

/* ---- audio: ring buffer for monitoring, peak meters ---- */

#define RING_FRAMES 32768 /* power of two, ~680 ms at 48 kHz */
static int16_t g_ring[RING_FRAMES * 2];
static volatile uint64_t g_ring_w, g_ring_r;
static int g_audio_rate = 48000;
static volatile int g_peak[2];

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    NSWindow *_window;
    DVCPreview *_preview;
    dvc100_t *_dev;
    CVPixelBufferPoolRef _pool;
    int _width, _height, _pal;
    volatile int _deinterlace;
    AVAudioEngine *_engine;
    CVPixelBufferRef _lastFrame;
    NSLock *_lastLock;

    os_unfair_lock _recLock;
    recorder_t *_rec;
    NSString *_recPath;
    NSDate *_recStart;
    int _recDeinterlaced;

    NSPopUpButton *_inputPopup, *_qualityPopup;
    NSButton *_deintCheck, *_monitorCheck, *_recordButton, *_folderButton;
    NSTextField *_timeLabel, *_statusLabel;
    NSLevelIndicator *_meterL, *_meterR;
    NSTimer *_uiTimer;
    NSURL *_folder;
}

static void frame_cb(const dvc100_frame_t *f, void *user)
{
    [(__bridge AppDelegate *)user pushFrame:f];
}

static void audio_cb(const int16_t *s, int frames, void *user)
{
    AppDelegate *self = (__bridge AppDelegate *)user;
    uint64_t w = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE);
    int pl = 0, pr = 0;
    for (int i = 0; i < frames; i++, w++) {
        int16_t l = s[2 * i], r = s[2 * i + 1];
        g_ring[(w % RING_FRAMES) * 2] = l;
        g_ring[(w % RING_FRAMES) * 2 + 1] = r;
        if (abs(l) > pl) pl = abs(l);
        if (abs(r) > pr) pr = abs(r);
    }
    __atomic_store_n(&g_ring_w, w, __ATOMIC_RELEASE);
    if (pl > g_peak[0]) g_peak[0] = pl;
    if (pr > g_peak[1]) g_peak[1] = pr;
    [self recordAudio:s frames:frames];
}

- (void)recordAudio:(const int16_t *)s frames:(int)frames
{
    os_unfair_lock_lock(&_recLock);
    if (_rec)
        rec_audio(_rec, s, frames);
    os_unfair_lock_unlock(&_recLock);
}

- (void)pushFrame:(const dvc100_frame_t *)f
{
    os_unfair_lock_lock(&_recLock);
    if (_rec)
        rec_video(_rec, f->data); /* full interlaced frame; ffmpeg deinterlaces properly */
    os_unfair_lock_unlock(&_recLock);

    CVPixelBufferRef pb = NULL;
    if (CVPixelBufferPoolCreatePixelBuffer(NULL, _pool, &pb) != kCVReturnSuccess)
        return;
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *dst = CVPixelBufferGetBaseAddress(pb);
    size_t dstride = CVPixelBufferGetBytesPerRow(pb), sstride = (size_t)f->width * 2;
    for (int y = 0; y < f->height; y++) {
        const uint8_t *src = f->data + (size_t)y * sstride;
        uint8_t *d = dst + (size_t)y * dstride;
        if (_deinterlace && y + 1 < f->height) {
            /* preview only: line blend, cheap and removes combing on motion */
            const uint8_t *next = src + sstride;
            for (size_t i = 0; i < sstride; i++)
                d[i] = (uint8_t)((src[i] + next[i] + 1) >> 1);
        } else {
            memcpy(d, src, sstride);
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);

    CMVideoFormatDescriptionRef fmt = NULL;
    CMVideoFormatDescriptionCreateForImageBuffer(NULL, pb, &fmt);
    CMSampleTimingInfo timing = { kCMTimeInvalid, CMClockGetTime(CMClockGetHostTimeClock()),
                                  kCMTimeInvalid };
    CMSampleBufferRef sb = NULL;
    CMSampleBufferCreateReadyWithImageBuffer(NULL, pb, fmt, &timing, &sb);
    if (sb) {
        CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, YES);
        CFMutableDictionaryRef d = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(atts, 0);
        CFDictionarySetValue(d, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        dispatch_async(dispatch_get_main_queue(), ^{
            AVSampleBufferVideoRenderer *renderer = self->_preview.displayLayer.sampleBufferRenderer;
            if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed)
                [renderer flush];
            [renderer enqueueSampleBuffer:sb];
            CFRelease(sb);
        });
    }
    if (fmt)
        CFRelease(fmt);

    [_lastLock lock];
    if (_lastFrame)
        CVPixelBufferRelease(_lastFrame);
    _lastFrame = pb;
    [_lastLock unlock];
}

/* ---- launch ---- */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    _lastLock = [NSLock new];
    _recLock = OS_UNFAIR_LOCK_INIT;
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    [ud registerDefaults:@{ @"input" : @0, @"deinterlace" : @YES, @"quality" : @1, @"monitor" : @YES }];
    NSString *folder = [ud stringForKey:@"folder"];
    _folder = folder ? [NSURL fileURLWithPath:folder]
                     : [NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory
                                                            inDomains:NSUserDomainMask].firstObject;
    _deinterlace = [ud boolForKey:@"deinterlace"];
    dvc100_input_t input = [ud integerForKey:@"input"] == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE;

    dvc100_norm_t norm = DVC100_NORM_AUTO;
    for (NSString *a in NSProcessInfo.processInfo.arguments) {
        if ([a isEqualToString:@"--svideo"]) input = DVC100_INPUT_SVIDEO;
        if ([a isEqualToString:@"--pal"]) norm = DVC100_NORM_PAL;
        if ([a isEqualToString:@"--ntsc"]) norm = DVC100_NORM_NTSC;
        if ([a isEqualToString:@"-v"]) dvc100_set_verbose(1);
    }

    [self buildMenu];
    int r = dvc100_open(&_dev, 0, 0);
    if (!r)
        r = dvc100_init(_dev, input, norm);
    if (r) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"No Dazzle capture device available";
        alert.informativeText = [NSString stringWithFormat:@"%s.\n\nCheck that the device is plugged in "
                                                           @"and not in use by another app.",
                                                           dvc100_strerror(r)];
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }

    _width = 720;
    _pal = dvc100_current_norm(_dev) == DVC100_NORM_PAL;
    _height = _pal ? 576 : 480;
    NSDictionary *attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_422YpCbCr8_yuvs),
        (id)kCVPixelBufferWidthKey : @(_width),
        (id)kCVPixelBufferHeightKey : @(_height),
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &_pool);

    [self buildWindow];
    [_inputPopup selectItemAtIndex:input == DVC100_INPUT_SVIDEO ? 1 : 0];

    r = dvc100_start(_dev, frame_cb, (__bridge void *)self);
    if (r)
        [self setStatus:[NSString stringWithFormat:@"Video streaming failed: %s", dvc100_strerror(r)]];
    [self startAudio];

    _uiTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 20 target:self selector:@selector(tick)
                                              userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_uiTimer forMode:NSRunLoopCommonModes];
}

- (NSTextField *)label:(NSString *)s
{
    NSTextField *l = [NSTextField labelWithString:s];
    l.textColor = NSColor.secondaryLabelColor;
    return l;
}

- (NSLevelIndicator *)meter
{
    NSLevelIndicator *m = [NSLevelIndicator new];
    m.levelIndicatorStyle = NSLevelIndicatorStyleContinuousCapacity;
    m.minValue = 0;
    m.maxValue = 60; /* dBFS + 60 */
    m.warningValue = 48;
    m.criticalValue = 57;
    [m.widthAnchor constraintEqualToConstant:90].active = YES;
    [m.heightAnchor constraintEqualToConstant:8].active = YES;
    return m;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 800, 600 + 90)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.delegate = self;
    _window.title = @"Dazzle Capture";
    _window.frameAutosaveName = @"DazzleCaptureWindow";

    _preview = [[DVCPreview alloc] initWithFrame:NSZeroRect];
    _preview.translatesAutoresizingMaskIntoConstraints = NO;

    /* row 1: source and recording settings */
    _inputPopup = [NSPopUpButton new];
    [_inputPopup addItemsWithTitles:@[ @"Composite", @"S-Video" ]];
    _inputPopup.target = self;
    _inputPopup.action = @selector(inputChanged:);

    _deintCheck = [NSButton checkboxWithTitle:@"Deinterlace" target:self action:@selector(deinterlaceChanged:)];
    _deintCheck.state = _deinterlace ? NSControlStateValueOn : NSControlStateValueOff;
    _deintCheck.toolTip = @"Preview and recording. Off records interlaced MPEG-2, as on a DVD.";

    _qualityPopup = [NSPopUpButton new];
    [_qualityPopup addItemsWithTitles:@[ @"Standard (6 Mbit/s)", @"High (8 Mbit/s)", @"Maximum (9.5 Mbit/s)" ]];
    [_qualityPopup selectItemAtIndex:[NSUserDefaults.standardUserDefaults integerForKey:@"quality"]];
    _qualityPopup.target = self;
    _qualityPopup.action = @selector(qualityChanged:);

    _folderButton = [NSButton buttonWithTitle:@"" target:self action:@selector(chooseFolder:)];
    _folderButton.bezelStyle = NSBezelStyleRounded;
    _folderButton.imagePosition = NSImageLeading;
    _folderButton.image = [NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:@"Folder"];
    [_folderButton.widthAnchor constraintLessThanOrEqualToConstant:220].active = YES;
    [self updateFolderButton];

    NSStackView *row1 = [NSStackView stackViewWithViews:@[
        [self label:@"Input"], _inputPopup, _deintCheck, [self label:@"Quality"], _qualityPopup,
        [self label:@"Save to"], _folderButton
    ]];
    row1.spacing = 8;
    [row1 setCustomSpacing:16 afterView:_inputPopup];
    [row1 setCustomSpacing:16 afterView:_deintCheck];
    [row1 setCustomSpacing:16 afterView:_qualityPopup];

    /* row 2: record button, time, meters, monitor */
    _recordButton = [NSButton buttonWithTitle:@"Record" target:self action:@selector(toggleRecord:)];
    _recordButton.bezelStyle = NSBezelStyleRounded;
    _recordButton.image = [NSImage imageWithSystemSymbolName:@"record.circle" accessibilityDescription:nil];
    _recordButton.imagePosition = NSImageLeading;
    _recordButton.contentTintColor = NSColor.systemRedColor;
    _recordButton.keyEquivalent = @"r";
    _recordButton.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [_recordButton.widthAnchor constraintGreaterThanOrEqualToConstant:110].active = YES;

    _timeLabel = [NSTextField labelWithString:@"00:00:00"];
    _timeLabel.font = [NSFont monospacedDigitSystemFontOfSize:15 weight:NSFontWeightMedium];

    _meterL = [self meter];
    _meterR = [self meter];
    NSStackView *meters = [NSStackView stackViewWithViews:@[
        [NSStackView stackViewWithViews:@[ [self label:@"L"], _meterL ]],
        [NSStackView stackViewWithViews:@[ [self label:@"R"], _meterR ]]
    ]];
    meters.orientation = NSUserInterfaceLayoutOrientationVertical;
    meters.spacing = 2;
    meters.alignment = NSLayoutAttributeLeading;

    _monitorCheck = [NSButton checkboxWithTitle:@"Play audio" target:self action:@selector(monitorChanged:)];
    _monitorCheck.state = [NSUserDefaults.standardUserDefaults boolForKey:@"monitor"] ? NSControlStateValueOn
                                                                                      : NSControlStateValueOff;

    _statusLabel = [NSTextField labelWithString:@""];
    _statusLabel.textColor = NSColor.secondaryLabelColor;
    _statusLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [_statusLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView *row2 = [NSStackView stackViewWithViews:@[ _recordButton, _timeLabel, meters, _monitorCheck, _statusLabel ]];
    row2.spacing = 16;

    NSStackView *bar = [NSStackView stackViewWithViews:@[ row1, row2 ]];
    bar.orientation = NSUserInterfaceLayoutOrientationVertical;
    bar.alignment = NSLayoutAttributeLeading;
    bar.spacing = 8;
    bar.edgeInsets = NSEdgeInsetsMake(10, 12, 10, 12);
    bar.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *content = [NSView new];
    [content addSubview:_preview];
    [content addSubview:bar];
    [NSLayoutConstraint activateConstraints:@[
        [_preview.topAnchor constraintEqualToAnchor:content.topAnchor],
        [_preview.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_preview.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [bar.topAnchor constraintEqualToAnchor:_preview.bottomAnchor],
        [bar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [bar.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        /* the picture is always 4:3; the window grows and shrinks around it */
        [_preview.heightAnchor constraintEqualToAnchor:_preview.widthAnchor multiplier:3.0 / 4.0],
        [_preview.widthAnchor constraintGreaterThanOrEqualToConstant:720],
    ]];
    [bar setHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
    _window.contentView = content;
    [self updateTitle];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)buildMenu
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Quit Dazzle Capture" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem *fileItem = [NSMenuItem new];
    [bar addItem:fileItem];
    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    [file addItemWithTitle:@"Start/Stop Recording" action:@selector(toggleRecord:) keyEquivalent:@"r"];
    [file addItemWithTitle:@"Save Snapshot" action:@selector(snapshot:) keyEquivalent:@"s"];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Choose Recording Folder…" action:@selector(chooseFolder:) keyEquivalent:@""];
    [file addItemWithTitle:@"Show Recordings in Finder" action:@selector(revealFolder:) keyEquivalent:@""];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    fileItem.submenu = file;

    NSMenuItem *viewItem = [NSMenuItem new];
    [bar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"]
        .keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = view;
    NSApp.mainMenu = bar;
}

- (void)updateTitle
{
    _window.title = [NSString stringWithFormat:@"Dazzle Capture — %@ %@", _inputPopup.titleOfSelectedItem ?: @"",
                                               _pal ? @"PAL 720×576" : @"NTSC 720×480"];
}

- (void)updateFolderButton
{
    _folderButton.title = _folder.lastPathComponent ?: @"Movies";
    _folderButton.toolTip = _folder.path;
}

- (void)setStatus:(NSString *)s
{
    _statusLabel.stringValue = s ?: @"";
}

/* ---- controls ---- */

- (void)inputChanged:(id)sender
{
    dvc100_input_t in = _inputPopup.indexOfSelectedItem == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE;
    dvc100_set_input(_dev, in);
    [NSUserDefaults.standardUserDefaults setInteger:_inputPopup.indexOfSelectedItem forKey:@"input"];
    [self updateTitle];
}

- (void)deinterlaceChanged:(id)sender
{
    _deinterlace = _deintCheck.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:_deinterlace forKey:@"deinterlace"];
}

- (void)qualityChanged:(id)sender
{
    [NSUserDefaults.standardUserDefaults setInteger:_qualityPopup.indexOfSelectedItem forKey:@"quality"];
}

- (void)monitorChanged:(id)sender
{
    BOOL on = _monitorCheck.state == NSControlStateValueOn;
    _engine.mainMixerNode.outputVolume = on ? 1 : 0;
    [NSUserDefaults.standardUserDefaults setBool:on forKey:@"monitor"];
}

- (void)chooseFolder:(id)sender
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.canCreateDirectories = YES;
    p.prompt = @"Choose";
    p.directoryURL = _folder;
    [p beginSheetModalForWindow:_window completionHandler:^(NSModalResponse res) {
        if (res != NSModalResponseOK)
            return;
        self->_folder = p.URL;
        [NSUserDefaults.standardUserDefaults setObject:p.URL.path forKey:@"folder"];
        [self updateFolderButton];
    }];
}

- (void)revealFolder:(id)sender
{
    [NSWorkspace.sharedWorkspace openURL:_folder];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(toggleRecord:))
        item.title = _rec ? @"Stop Recording" : @"Start Recording";
    return YES;
}

/* ---- recording ---- */

- (void)toggleRecord:(id)sender
{
    if (_rec)
        [self stopRecording];
    else
        [self startRecording];
}

- (void)startRecording
{
    static const int kbps[] = { 6000, 8000, 9500 };
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *name = [NSString stringWithFormat:@"Dazzle Capture %@.mpg", [df stringFromDate:NSDate.date]];
    NSString *path = [_folder URLByAppendingPathComponent:name].path;

    recorder_opts_t o = {
        .width = _width,
        .height = _height,
        .pal = _pal,
        .deinterlace = _deinterlace,
        .video_kbps = kbps[MAX(0, MIN(2, (int)_qualityPopup.indexOfSelectedItem))],
        .audio_kbps = 224,
        .audio_rate = g_audio_rate,
    };
    char err[256] = "";
    recorder_t *rec = rec_open(path.fileSystemRepresentation, &o, err, sizeof(err));
    if (!rec) {
        NSAlert *a = [NSAlert new];
        a.messageText = @"Cannot start recording";
        a.informativeText = [NSString stringWithUTF8String:err];
        [a beginSheetModalForWindow:_window completionHandler:nil];
        return;
    }
    os_unfair_lock_lock(&_recLock);
    _rec = rec;
    os_unfair_lock_unlock(&_recLock);
    _recPath = path;
    _recStart = NSDate.date;
    _recDeinterlaced = o.deinterlace;

    _recordButton.title = @"Stop";
    _recordButton.image = [NSImage imageWithSystemSymbolName:@"stop.circle.fill" accessibilityDescription:nil];
    _deintCheck.enabled = _qualityPopup.enabled = _folderButton.enabled = NO;
    _preview.badge.hidden = NO;
    [self setStatus:[NSString stringWithFormat:@"Recording %@ MPEG-2 to %@",
                                               o.deinterlace ? @"deinterlaced" : @"interlaced", name]];
}

- (void)stopRecording
{
    os_unfair_lock_lock(&_recLock);
    recorder_t *rec = _rec;
    _rec = NULL;
    os_unfair_lock_unlock(&_recLock);
    if (!rec)
        return;

    NSString *path = _recPath;
    _recordButton.enabled = NO;
    _recordButton.title = @"Finishing…";
    [self setStatus:@"Finishing the file…"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int r = rec_close(rec);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_recordButton.enabled = YES;
            self->_recordButton.title = @"Record";
            self->_recordButton.image = [NSImage imageWithSystemSymbolName:@"record.circle"
                                                  accessibilityDescription:nil];
            self->_deintCheck.enabled = self->_qualityPopup.enabled = self->_folderButton.enabled = YES;
            self->_preview.badge.hidden = YES;
            if (r == 0) {
                [self setStatus:[@"Saved " stringByAppendingString:path.lastPathComponent]];
                [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path] ]];
            } else {
                [self setStatus:@"Recording failed (ffmpeg error), see Console for details"];
            }
        });
    });
}

- (void)tick
{
    /* meters, with a little decay so peaks stay visible */
    static double lv[2];
    for (int c = 0; c < 2; c++) {
        int p = __atomic_exchange_n(&g_peak[c], 0, __ATOMIC_RELAXED);
        double db = p > 0 ? 20 * log10(p / 32768.0) : -60;
        double v = MAX(0, db + 60);
        lv[c] = MAX(v, lv[c] - 1.5);
    }
    _meterL.doubleValue = lv[0];
    _meterR.doubleValue = lv[1];

    if (_rec) {
        NSTimeInterval t = -_recStart.timeIntervalSinceNow;
        int s = (int)t;
        _timeLabel.stringValue = [NSString stringWithFormat:@"%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60];
        _preview.badge.stringValue = [NSString stringWithFormat:@"● REC %@", _timeLabel.stringValue];
        [_preview setNeedsLayout:YES];
    } else if (!_recordButton.enabled) {
        /* finishing */
    } else {
        _timeLabel.stringValue = @"00:00:00";
    }
}

/* ---- snapshot ---- */

- (void)snapshot:(id)sender
{
    [_lastLock lock];
    CVPixelBufferRef pb = _lastFrame ? CVPixelBufferRetain(_lastFrame) : NULL;
    [_lastLock unlock];
    if (!pb)
        return;
    CIImage *img = [CIImage imageWithCVPixelBuffer:pb];
    img = [img imageByApplyingTransform:CGAffineTransformMakeScale(640.0 / _width, 480.0 / _height)];
    CIContext *ctx = [CIContext context];
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *name = [NSString stringWithFormat:@"Dazzle Capture %@.png", [df stringFromDate:NSDate.date]];
    NSURL *url = [_folder URLByAppendingPathComponent:name];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    NSError *err = nil;
    [ctx writePNGRepresentationOfImage:img toURL:url format:kCIFormatRGBA8 colorSpace:cs options:@{} error:&err];
    CGColorSpaceRelease(cs);
    CVPixelBufferRelease(pb);
    [self setStatus:err ? err.localizedDescription : [@"Saved " stringByAppendingString:name]];
}

/* ---- audio ----
 * The DVC100's UAC1 stream is not published by AppleUSBAudio, so it arrives
 * through libusb; the black Dazzle's comes from its Core Audio device. Either
 * way it is played via a ring buffer. */

- (void)startAudio
{
    int r = dvc100_audio_start(_dev, audio_cb, (__bridge void *)self, &g_audio_rate);
    if (r) {
        [self setStatus:[NSString stringWithFormat:@"Audio unavailable: %s", dvc100_strerror(r)]];
        return;
    }
    _engine = [AVAudioEngine new];
    AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:g_audio_rate channels:2];
    const uint64_t target = (uint64_t)g_audio_rate * 60 / 1000, maxlag = (uint64_t)g_audio_rate / 4;
    AVAudioSourceNode *src = [[AVAudioSourceNode alloc]
        initWithFormat:fmt
           renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount n,
                                 AudioBufferList *abl) {
               (void)ts;
               float *L = abl->mBuffers[0].mData, *R = abl->mBuffers[1].mData;
               uint64_t w = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE), rd = g_ring_r;
               if (w - rd > maxlag || rd > w)
                   rd = w > target ? w - target : 0; /* drifted too far behind: resync */
               int any = 0;
               for (AVAudioFrameCount i = 0; i < n; i++) {
                   if (rd < w) {
                       L[i] = g_ring[(rd % RING_FRAMES) * 2] / 32768.0f;
                       R[i] = g_ring[(rd % RING_FRAMES) * 2 + 1] / 32768.0f;
                       rd++;
                       any = 1;
                   } else {
                       L[i] = R[i] = 0;
                   }
               }
               g_ring_r = rd;
               *silence = !any;
               return noErr;
           }];
    [_engine attachNode:src];
    NSError *err = nil;
    [_engine connect:src to:_engine.mainMixerNode format:fmt];
    _engine.mainMixerNode.outputVolume = _monitorCheck.state == NSControlStateValueOn ? 1 : 0;
    g_ring_r = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE);
    if (![_engine startAndReturnError:&err])
        [self setStatus:[@"Audio output: " stringByAppendingString:err.localizedDescription]];
}

/* ---- shutdown ---- */

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    if (!_rec)
        return NSTerminateNow;
    NSAlert *a = [NSAlert new];
    a.messageText = @"A recording is in progress";
    a.informativeText = @"Stop the recording and quit?";
    [a addButtonWithTitle:@"Stop and Quit"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn)
        return NSTerminateCancel;
    os_unfair_lock_lock(&_recLock);
    recorder_t *rec = _rec;
    _rec = NULL;
    os_unfair_lock_unlock(&_recLock);
    rec_close(rec);
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)applicationWillTerminate:(NSNotification *)note
{
    [_uiTimer invalidate];
    [_engine stop];
    if (_dev)
        dvc100_close(_dev);
    _dev = NULL;
}
@end

int main(void)
{
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
