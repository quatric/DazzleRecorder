/*
 * DazzleRecorder: live preview and MPEG-2 recording for the Dazzle DVC100.
 *
 * The control bar under the preview selects the input, deinterlacing,
 * recording quality and folder, and starts/stops recording to a
 * DVD-compatible MPEG-2 program stream (.mpg).
 */
#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AudioUnit/AudioUnit.h>
#include <pthread.h>

#include "dvc100.h"
#include "monitor_ring.h"
#include "recorder.h"
#include "stopafter.h"
#include "sysaudio.h"

/* ---- AudioComponent / AudioUnit calls, looked up at run time ----
 * Current SDKs bind these functions to AudioToolbox, but older macOS (10.9 and 10.10) exports
 * them from AudioUnit.framework instead, so a direct call cannot be bound there and dyld kills
 * the app the first time one runs. Look them up by name, wherever this macOS keeps them. */
#include <dlfcn.h>

static void *au_sym(const char *name)
{
    static void *au_lib;
    static int tried;
    if (!tried) {
        tried = 1;
        au_lib = dlopen("/System/Library/Frameworks/AudioUnit.framework/AudioUnit", RTLD_LAZY | RTLD_GLOBAL);
    }
    void *f = au_lib ? dlsym(au_lib, name) : NULL;
    return f ? f : dlsym(RTLD_DEFAULT, name);
}

typedef AudioComponent (*au_find_next_t)(AudioComponent, const AudioComponentDescription *);
typedef OSStatus (*au_instance_new_t)(AudioComponent, AudioComponentInstance *);
typedef OSStatus (*au_unit_fn_t)(AudioUnit); /* Dispose, Initialize, OutputUnitStart/Stop */
typedef OSStatus (*au_set_property_t)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *,
                                      UInt32);

static AudioComponent au_find_next(AudioComponent c, const AudioComponentDescription *d)
{
    static au_find_next_t f;
    if (!f)
        f = (au_find_next_t)au_sym("AudioComponentFindNext");
    return f ? f(c, d) : NULL;
}
static OSStatus au_instance_new(AudioComponent c, AudioComponentInstance *out)
{
    static au_instance_new_t f;
    if (!f)
        f = (au_instance_new_t)au_sym("AudioComponentInstanceNew");
    return f ? f(c, out) : kAudio_UnimplementedError;
}
static OSStatus au_set_property(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el,
                                const void *data, UInt32 size)
{
    static au_set_property_t f;
    if (!f)
        f = (au_set_property_t)au_sym("AudioUnitSetProperty");
    return f ? f(u, id, sc, el, data, size) : kAudio_UnimplementedError;
}
static OSStatus au_unit_call(const char *name, au_unit_fn_t *cache, AudioUnit u)
{
    if (!*cache)
        *cache = (au_unit_fn_t)au_sym(name);
    return *cache ? (*cache)(u) : kAudio_UnimplementedError;
}
static OSStatus au_instance_dispose(AudioComponentInstance u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioComponentInstanceDispose", &f, u);
}
static OSStatus au_initialize(AudioUnit u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioUnitInitialize", &f, u);
}
static OSStatus au_output_start(AudioUnit u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioOutputUnitStart", &f, u);
}
static OSStatus au_output_stop(AudioUnit u)
{
    static au_unit_fn_t f;
    return au_unit_call("AudioOutputUnitStop", &f, u);
}


/* ---- AppKit helpers: the modern convenience constructors need 10.12+, SF Symbols 11+ ---- */

/* ---- 10.9 compatibility: the API added in 10.10 and 10.11 is used only where it exists ---- */

static NSColor *label_color(BOOL secondary)
{
    if (@available(macOS 10.10, *))
        return secondary ? [NSColor secondaryLabelColor] : [NSColor labelColor];
    return secondary ? NSColor.disabledControlTextColor : NSColor.controlTextColor;
}

static NSFont *mono_font(CGFloat size, BOOL semibold)
{
    if (@available(macOS 10.11, *))
        return [NSFont monospacedDigitSystemFontOfSize:size
                                                weight:semibold ? NSFontWeightSemibold : NSFontWeightMedium];
    return [NSFont userFixedPitchFontOfSize:size];
}

/* layout anchors and -setActive: need 10.11 / 10.10, so constraints are built the 10.7 way */
static NSLayoutConstraint *mk(id a, NSLayoutAttribute aa, NSLayoutRelation rel, id b, NSLayoutAttribute ba,
                              CGFloat mult, CGFloat c)
{
    return [NSLayoutConstraint constraintWithItem:a attribute:aa relatedBy:rel toItem:b
                                        attribute:b ? ba : NSLayoutAttributeNotAnAttribute
                                       multiplier:mult constant:c];
}

static void set_active(NSLayoutConstraint *c, BOOL on)
{
    if (!c)
        return;
    if (@available(macOS 10.10, *)) {
        c.active = on;
        return;
    }
    id a = c.firstItem, b = c.secondItem;
    NSView *owner = b ? [a ancestorSharedWithView:b] : a;
    if (on)
        [owner addConstraint:c];
    else
        [owner removeConstraint:c];
}

static void activate_all(NSArray<NSLayoutConstraint *> *cs)
{
    for (NSLayoutConstraint *c in cs)
        set_active(c, YES);
}

static NSTextField *make_label(NSString *s)
{
    if (@available(macOS 10.12, *))
        return [NSTextField labelWithString:s];
    NSTextField *l = [NSTextField new];
    l.stringValue = s;
    l.bezeled = NO;
    l.drawsBackground = NO;
    l.editable = NO;
    l.selectable = NO;
    l.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    l.textColor = label_color(NO);
    [l.cell setLineBreakMode:NSLineBreakByClipping];
    return l;
}

static char kCheckbox;

static NSButton *make_button(NSString *title, id target, SEL action, BOOL checkbox)
{
    if (@available(macOS 10.12, *)) {
        NSButton *b = checkbox ? [NSButton checkboxWithTitle:title target:target action:action]
                               : [NSButton buttonWithTitle:title target:target action:action];
        if (checkbox)
            objc_setAssociatedObject(b, &kCheckbox, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return b;
    }
    NSButton *b = [NSButton new];
    b.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    b.title = title;
    b.target = target;
    b.action = action;
    if (checkbox) {
        objc_setAssociatedObject(b, &kCheckbox, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [b setButtonType:NSButtonTypeSwitch];
    } else {
        [b setButtonType:NSButtonTypeMomentaryPushIn];
        b.bezelStyle = NSBezelStyleRounded;
    }
    return b;
}

static void set_symbol(NSButton *b, NSString *name, NSString *desc)
{
    if (@available(macOS 11.0, *)) {
        b.image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:desc];
        b.imagePosition = NSImageLeading;
    }
}

/*
 * The control bar is laid out by this small row/column view rather than NSStackView: 10.9's NSStackView
 * sizes and clips its views differently from 10.10 and later, and there it left the bar with no room
 * under the preview. A Box measures its views (their intrinsic sizes, or the limits set with
 * limit_size) and places them by frame, which works the same on every macOS since 10.7.
 */
static char kSizeLimits;

/* min and max width (0: none) and a fixed height (0: its own) for a view in a Box */
static void limit_size(NSView *v, CGFloat minw, CGFloat maxw, CGFloat h)
{
    objc_setAssociatedObject(v, &kSizeLimits, @[ @(minw), @(maxw), @(h) ], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

@interface Box : NSView
@property(nonatomic) NSUserInterfaceLayoutOrientation orientation; /* rows center their views, columns align them left */
@property(nonatomic) CGFloat spacing;
@property(nonatomic) NSEdgeInsets edgeInsets;
@property(nonatomic, weak) NSView *flexible; /* in a row: gets the width the others leave */
- (void)setGap:(CGFloat)gap afterView:(NSView *)v;
- (NSSize)naturalSize;
- (void)place;
- (void)refit; /* after a view's title changes */
@end

static NSSize natural_size(NSView *v)
{
    if ([v isKindOfClass:Box.class])
        return [(Box *)v naturalSize];
    NSSize s = v.intrinsicContentSize; /* -1 is NSViewNoIntrinsicMetric, a variable only since 10.11 */
    /* a label's cell adds the few points of padding its intrinsic width leaves out */
    if ([v isKindOfClass:NSTextField.class])
        s.width = MAX(s.width, [[(NSTextField *)v cell] cellSize].width);
    /* 10.9 to 10.11 measure checkbox and button titles a few points short, which cut off their last
     * letter ("Widescree"): measure the title itself and add the box or bezel around it */
    if ([v isKindOfClass:NSPopUpButton.class]) {
        s.width = MAX(s.width, [[(NSPopUpButton *)v cell] cellSize].width);
    } else if ([v isKindOfClass:NSButton.class]) {
        NSButton *b = (NSButton *)v;
        BOOL box = objc_getAssociatedObject(b, &kCheckbox) != nil;
        CGFloat title = ceil([b.attributedTitle size].width);
        CGFloat image = b.image && b.imagePosition != NSNoImage ? b.image.size.width + 4 : 0;
        s.width = MAX(s.width, MAX([b.cell cellSize].width, title + image + (box ? 24 : 28)));
    }
    if (s.width < 0)
        s.width = v.frame.size.width;
    if (s.height < 0)
        s.height = v.frame.size.height;
    NSArray<NSNumber *> *lim = objc_getAssociatedObject(v, &kSizeLimits);
    if (lim) {
        s.width = MAX(s.width, lim[0].doubleValue);
        if (lim[1].doubleValue > 0)
            s.width = MIN(s.width, lim[1].doubleValue);
        if (lim[2].doubleValue > 0)
            s.height = lim[2].doubleValue;
    }
    return NSMakeSize(ceil(s.width), ceil(s.height));
}

@implementation Box {
    NSMutableDictionary<NSValue *, NSNumber *> *_gaps;
}
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _orientation = NSUserInterfaceLayoutOrientationHorizontal;
        _spacing = 8;
        _gaps = [NSMutableDictionary new];
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (void)setGap:(CGFloat)gap afterView:(NSView *)v
{
    _gaps[[NSValue valueWithNonretainedObject:v]] = @(gap);
}
- (CGFloat)gapAfter:(NSView *)v
{
    NSNumber *g = _gaps[[NSValue valueWithNonretainedObject:v]];
    return g ? g.doubleValue : _spacing;
}
- (BOOL)horizontal { return _orientation == NSUserInterfaceLayoutOrientationHorizontal; }
- (NSSize)naturalSize
{
    CGFloat along = 0, across = 0;
    NSArray<NSView *> *views = self.subviews;
    for (NSView *v in views) {
        NSSize s = natural_size(v);
        if (v == _flexible)
            s.width = 0;
        along += self.horizontal ? s.width : s.height;
        across = MAX(across, self.horizontal ? s.height : s.width);
        if (v != views.lastObject)
            along += [self gapAfter:v];
    }
    NSEdgeInsets in = _edgeInsets;
    return self.horizontal ? NSMakeSize(along + in.left + in.right, across + in.top + in.bottom)
                           : NSMakeSize(across + in.left + in.right, along + in.top + in.bottom);
}
/* the bar keeps the window at least as wide as its rows (the flexible label aside) */
- (NSSize)intrinsicContentSize
{
    return self.naturalSize;
}
- (void)place
{
    NSEdgeInsets in = _edgeInsets;
    NSRect r = NSMakeRect(in.left, in.top, MAX(0, self.bounds.size.width - in.left - in.right),
                          MAX(0, self.bounds.size.height - in.top - in.bottom));
    NSArray<NSView *> *views = self.subviews;
    CGFloat extra = r.size.width;
    if (self.horizontal) {
        for (NSView *v in views) {
            if (v != _flexible)
                extra -= natural_size(v).width;
            if (v != views.lastObject)
                extra -= [self gapAfter:v];
        }
    }
    CGFloat pos = self.horizontal ? NSMinX(r) : NSMinY(r);
    for (NSView *v in views) {
        NSSize s = natural_size(v);
        NSRect f;
        if (self.horizontal) {
            if (v == _flexible)
                s.width = MAX(0, extra);
            f = NSMakeRect(pos, NSMinY(r) + round((r.size.height - s.height) / 2), s.width, s.height);
            pos += s.width;
        } else {
            if ([v isKindOfClass:Box.class])
                s.width = r.size.width; /* a row in a column gets the column's width */
            f = NSMakeRect(NSMinX(r), pos, s.width, s.height);
            pos += s.height;
        }
        pos += [self gapAfter:v];
        v.frame = f;
        if ([v isKindOfClass:Box.class])
            [(Box *)v place];
    }
}
- (void)layout
{
    [super layout];
    [self place];
}
- (void)refit
{
    [self invalidateIntrinsicContentSize];
    self.needsLayout = YES;
}
- (void)resizeSubviewsWithOldSize:(NSSize)old
{
    (void)old;
    [self place];
}
@end

static Box *stack(NSArray<NSView *> *views)
{
    Box *b = [[Box alloc] initWithFrame:NSZeroRect];
    for (NSView *v in views) {
        v.translatesAutoresizingMaskIntoConstraints = YES;
        v.autoresizingMask = NSViewNotSizable;
        [b addSubview:v];
    }
    return b;
}

/* ---- preview view: keeps the picture at 4:3 (or stretched to 16:9) inside whatever space it has ---- */

@interface DVCPreview : NSView
@property(nonatomic, strong) AVSampleBufferDisplayLayer *displayLayer;
@property(nonatomic, strong) NSTextField *badge, *message;
@property(nonatomic) BOOL widescreen;
@end

@implementation DVCPreview
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = NSColor.blackColor.CGColor;
        _displayLayer = [AVSampleBufferDisplayLayer layer];
        /* 720 BT.601 samples span a 4:3 picture, so stretch into a 4:3 (or 16:9) rect */
        _displayLayer.videoGravity = AVLayerVideoGravityResize;
        [self.layer addSublayer:_displayLayer];

        _badge = make_label(@"");
        _badge.font = mono_font(13, YES);
        _badge.textColor = NSColor.whiteColor;
        _badge.backgroundColor = [NSColor colorWithRed:0.8 green:0.05 blue:0.05 alpha:0.85];
        _badge.drawsBackground = YES;
        _badge.hidden = YES;
        [self addSubview:_badge];

        _message = make_label(@"Preview off");
        _message.font = [NSFont systemFontOfSize:15];
        _message.textColor = [NSColor colorWithWhite:0.7 alpha:1];
        _message.hidden = YES;
        [self addSubview:_message];
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (void)setWidescreen:(BOOL)on
{
    _widescreen = on;
    [self setNeedsLayout:YES];
}
- (void)layout
{
    [super layout];
    NSSize s = self.bounds.size;
    CGFloat ar = _widescreen ? 16.0 / 9 : 4.0 / 3;
    CGFloat w = s.width, h = w / ar;
    if (h > s.height) {
        h = s.height;
        w = h * ar;
    }
    NSRect r = NSMakeRect((s.width - w) / 2, (s.height - h) / 2, w, h);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _displayLayer.frame = r;
    [CATransaction commit];
    [_badge sizeToFit];
    _badge.frame = NSMakeRect(NSMinX(r) + 10, NSMinY(r) + 10, _badge.frame.size.width + 8, 20);
    [_message sizeToFit];
    _message.frame = NSMakeRect(NSMidX(r) - _message.frame.size.width / 2, NSMidY(r) - _message.frame.size.height / 2,
                                _message.frame.size.width, _message.frame.size.height);
}
@end

/* ---- audio: ring buffer for monitoring, peak meters ---- */

#define RING_FRAMES 32768 /* power of two, ~680 ms at 48 kHz */
static int16_t g_ring_frames[RING_FRAMES * 2];
static monitor_ring_t g_ring = { g_ring_frames, RING_FRAMES, 0, 0, 0, 0 };
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
    int _audioOK;                         /* audio input running; otherwise recordings get silence */
    uint64_t _silenceFrames, _silenceFed;
    volatile int _deinterlace;
    volatile int _previewOn; /* 0: frames are not converted or shown (recording is unaffected) */
    int _widescreen;
    NSLayoutConstraint *_aspectConstraint;
    id _engine; /* AVAudioEngine, 10.15 and later only */
    CVPixelBufferRef _lastFrame;
    NSLock *_lastLock;
    BOOL _timed;     /* the preview layer runs on a host-clock timebase: frames carry their display time */
    double _nextPts; /* display time of the next frame, in host-clock seconds */

    pthread_mutex_t _recLock;
    AudioUnit _outputUnit; /* monitor output before 10.15 (no AVAudioSourceNode) */
    recorder_t *_rec;
    NSString *_recPath;
    NSDate *_recStart;
    int _recDeinterlaced;

    NSPopUpButton *_inputPopup, *_formatPopup, *_qualityPopup, *_audioPopup;
    NSComboBox *_stopAfterPopup;
    int _stopAfterMin; /* minutes, 0 = no limit */
    NSButton *_deintCheck, *_wideCheck, *_previewCheck, *_monitorCheck, *_recordButton, *_folderButton;
    Box *_bar;
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
    monitor_ring_write(&g_ring, s, frames, g_peak);
    [self recordAudio:s frames:frames];
}

- (void)recordAudio:(const int16_t *)s frames:(int)frames
{
    pthread_mutex_lock(&_recLock);
    if (_rec)
        rec_audio(_rec, s, frames);
    pthread_mutex_unlock(&_recLock);
}

- (void)pushFrame:(const dvc100_frame_t *)f
{
    pthread_mutex_lock(&_recLock);
    if (_rec)
    {
        rec_video(_rec, f->data); /* full interlaced frame; ffmpeg deinterlaces properly */
        if (!_audioOK) {
            /* no audio input (microphone refused, device missing): feed 48 kHz silence so the file still
             * has an audio clock instead of ffmpeg waiting for audio that never comes */
            static const int16_t zeros[2 * 1922] = { 0 };
            uint64_t due = ++_silenceFrames * (_pal ? 1920 : 8008) / (_pal ? 1 : 5);
            rec_audio(_rec, zeros, (int)(due - _silenceFed));
            _silenceFed = due;
        }
    }
    pthread_mutex_unlock(&_recLock);

    if (!_previewOn)
        return;
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

    /* Frames come off USB in bursts (a transfer holds 8 ms of packets), so showing each one the moment it
     * lands puts some on the wrong screen refresh and the picture judders. Instead each frame gets a display
     * time a little ahead (two frames), smoothed over the last few arrivals so it moves by exactly one frame
     * period, and the layer shows it on the refresh where it falls. */
    const double period = _pal ? 1.0 / 25 : 1001.0 / 30000;
    double now = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())), want = now + 2 * period;
    double pts = _nextPts == 0 || fabs(want - _nextPts) > 0.25 ? want : _nextPts + (want - _nextPts) / 16;
    _nextPts = pts + period;

    CMVideoFormatDescriptionRef fmt = NULL;
    CMVideoFormatDescriptionCreateForImageBuffer(NULL, pb, &fmt);
    CMSampleTimingInfo timing = { CMTimeMakeWithSeconds(period, 1000000), CMTimeMakeWithSeconds(pts, 1000000000),
                                  kCMTimeInvalid };
    CMSampleBufferRef sb = NULL;
    CMSampleBufferCreateForImageBuffer(NULL, pb, true, NULL, NULL, fmt, &timing, &sb);
    if (sb) {
        if (!_timed) { /* no timebase: show it now */
            CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, YES);
            CFMutableDictionaryRef d = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(atts, 0);
            CFDictionarySetValue(d, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            AVSampleBufferDisplayLayer *layer = self->_preview.displayLayer;
            if (@available(macOS 14.0, *)) {
                AVSampleBufferVideoRenderer *renderer = layer.sampleBufferRenderer;
                if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed)
                    [renderer flush];
                [renderer enqueueSampleBuffer:sb];
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                if (@available(macOS 10.10, *)) {
                    if (layer.status == AVQueuedSampleBufferRenderingStatusFailed)
                        [layer flush];
                }
                [layer enqueueSampleBuffer:sb];
#pragma clang diagnostic pop
            }
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
    pthread_mutex_init(&_recLock, NULL);
    NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
    [ud registerDefaults:@{ @"input" : @0, @"deinterlace" : @YES, @"widescreen" : @NO, @"preview" : @YES, @"quality" : @1, @"monitor" : @YES, @"audioSource" : @0, @"format" : @0 }];
    NSString *folder = [ud stringForKey:@"folder"];
    _folder = folder ? [NSURL fileURLWithPath:folder]
                     : [NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory
                                                            inDomains:NSUserDomainMask].firstObject;
    _deinterlace = [ud boolForKey:@"deinterlace"];
    _widescreen = [ud boolForKey:@"widescreen"];
    _previewOn = [ud boolForKey:@"preview"];
    dvc100_input_t input = [ud integerForKey:@"input"] == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE;

    dvc100_norm_t norm = DVC100_NORM_AUTO;
    for (NSString *a in NSProcessInfo.processInfo.arguments) {
        if ([a isEqualToString:@"--svideo"]) input = DVC100_INPUT_SVIDEO;
        if ([a isEqualToString:@"--pal"]) norm = DVC100_NORM_PAL;
        if ([a isEqualToString:@"--ntsc"]) norm = DVC100_NORM_NTSC;
        if ([a isEqualToString:@"-v"]) dvc100_set_verbose(1);
    }

    sa_set_line_in((int)[ud integerForKey:@"audioSource"] == 1);
    for (NSString *a in NSProcessInfo.processInfo.arguments)
        if ([a isEqualToString:@"--line-in"]) sa_set_line_in(1);

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

    /* the preview layer's clock is the host clock, so frames can say when to appear (see pushFrame) */
    CMClockRef host = CMClockGetHostTimeClock();
    CMTimebaseRef tb = NULL;
    OSStatus tst;
    if (@available(macOS 12.0, *)) {
        tst = CMTimebaseCreateWithSourceClock(NULL, host, &tb);
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        tst = CMTimebaseCreateWithMasterClock(NULL, host, &tb);
#pragma clang diagnostic pop
    }
    if (tst == noErr && tb) {
        CMTimebaseSetTime(tb, CMClockGetTime(host));
        CMTimebaseSetRate(tb, 1.0);
        _preview.displayLayer.controlTimebase = tb;
        CFRelease(tb);
        _timed = YES;
    }

    r = dvc100_start(_dev, frame_cb, (__bridge void *)self);
    if (r)
        [self setStatus:[NSString stringWithFormat:@"Video streaming failed: %s", dvc100_strerror(r)]];
    [self requestMicrophoneThen:^{ [self startAudio]; }];

    _uiTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 20 target:self selector:@selector(tick)
                                              userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_uiTimer forMode:NSRunLoopCommonModes];
}

- (NSTextField *)label:(NSString *)s
{
    NSTextField *l = make_label(s);
    l.textColor = label_color(YES);
    return l;
}

- (NSLevelIndicator *)meter
{
    NSLevelIndicator *m = [NSLevelIndicator new];
    [(NSLevelIndicatorCell *)m.cell setLevelIndicatorStyle:NSLevelIndicatorStyleContinuousCapacity];
    m.minValue = 0;
    m.maxValue = 60; /* dBFS + 60 */
    m.warningValue = 48;
    m.criticalValue = 57;
    limit_size(m, 90, 90, 8);
    return m;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, MIN(560, NSScreen.mainScreen.visibleFrame.size.width - 40), MIN(420 + 150, NSScreen.mainScreen.visibleFrame.size.height - 60))
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.delegate = self;
    _window.title = @"DazzleRecorder";
    _window.frameAutosaveName = @"DazzleCaptureWindow2"; /* the old name remembered a larger window */

    _preview = [[DVCPreview alloc] initWithFrame:NSZeroRect];
    _preview.translatesAutoresizingMaskIntoConstraints = NO;

    /* row 1: source and recording settings */
    _inputPopup = [NSPopUpButton new];
    [_inputPopup addItemsWithTitles:@[ @"Composite", @"S-Video" ]];
    _inputPopup.target = self;
    _inputPopup.action = @selector(inputChanged:);

    _deintCheck = make_button(@"Deinterlace", self, @selector(deinterlaceChanged:), YES);
    _deintCheck.state = _deinterlace ? NSControlStateValueOn : NSControlStateValueOff;
    _deintCheck.toolTip = @"Preview and recording. Off records interlaced MPEG-2, as on a DVD.";

    _wideCheck = make_button(@"Widescreen", self, @selector(widescreenChanged:), YES);
    _wideCheck.state = _widescreen ? NSControlStateValueOn : NSControlStateValueOff;
    _wideCheck.toolTip = @"Stretch the picture to 16:9 in the preview, snapshots and recordings.";

    _previewCheck = make_button(@"Preview", self, @selector(previewChanged:), YES);
    _previewCheck.state = _previewOn ? NSControlStateValueOn : NSControlStateValueOff;
    _previewCheck.toolTip = @"Show the picture. Off saves the computer the work of drawing it; recording and sound go on.";

    _formatPopup = [NSPopUpButton new];
    [_formatPopup addItemsWithTitles:@[ @"MPEG-2", @"H.264" ]];
    [_formatPopup selectItemAtIndex:MAX(0, MIN(1, [NSUserDefaults.standardUserDefaults integerForKey:@"format"]))];
    _formatPopup.target = self;
    _formatPopup.action = @selector(formatChanged:);
    _formatPopup.toolTip = @"MPEG-2: a DVD-compatible .mpg, best for burning a DVD. "
                           @"H.264: a much smaller .mp4 (x264), best for keeping on a computer or sharing.";

    _qualityPopup = [NSPopUpButton new];
    [self fillQualityPopup];
    _qualityPopup.target = self;
    _qualityPopup.action = @selector(qualityChanged:);

    _folderButton = make_button(@"", self, @selector(chooseFolder:), NO);
    _folderButton.bezelStyle = NSBezelStyleRounded;
    set_symbol(_folderButton, @"folder", @"Folder");
    limit_size(_folderButton, 0, 220, 0);
    [self updateFolderButton];

    /* short rows rather than two long ones, so the window can be narrow */
    Box *row1 = stack(@[
        [self label:@"Input"], _inputPopup, [self label:@"Format"], _formatPopup, [self label:@"Quality"], _qualityPopup
    ]);
    row1.spacing = 8;
    [row1 setGap:16 afterView:_inputPopup];
    [row1 setGap:16 afterView:_formatPopup];

    /* row 2: record button, time, meters, monitor */
    _recordButton = make_button(@"Record", self, @selector(toggleRecord:), NO);
    _recordButton.bezelStyle = NSBezelStyleRounded;
    set_symbol(_recordButton, @"record.circle", nil);
    if (@available(macOS 10.14, *))
        _recordButton.contentTintColor = NSColor.systemRedColor;
    _recordButton.keyEquivalent = @"r";
    _recordButton.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    limit_size(_recordButton, 110, 0, 0);

    _timeLabel = make_label(@"00:00:00");
    _timeLabel.font = mono_font(15, NO);

    _meterL = [self meter];
    _meterR = [self meter];
    Box *meters = stack(@[
        stack(@[ [self label:@"L"], _meterL ]),
        stack(@[ [self label:@"R"], _meterR ])
    ]);
    meters.orientation = NSUserInterfaceLayoutOrientationVertical;
    meters.spacing = 2;

    _monitorCheck = make_button(@"Play audio", self, @selector(monitorChanged:), YES);
    _monitorCheck.state = [NSUserDefaults.standardUserDefaults boolForKey:@"monitor"] ? NSControlStateValueOn
                                                                                      : NSControlStateValueOff;

    _statusLabel = make_label(@"");
    _statusLabel.textColor = label_color(YES);
    [_statusLabel.cell setLineBreakMode:NSLineBreakByTruncatingMiddle];

    _audioPopup = [NSPopUpButton new];
    [_audioPopup addItemsWithTitles:@[ @"Dazzle", @"Line In" ]];
    [_audioPopup selectItemAtIndex:sa_line_in() ? 1 : 0];
    _audioPopup.target = self;
    _audioPopup.action = @selector(audioSourceChanged:);
    _audioPopup.toolTip = @"Record and play the sound from the Dazzle, or from this computer's Line In instead "
                          @"(the Dazzle's own audio is then bypassed).";

    _stopAfterPopup = [NSComboBox new];
    for (int i = 0; i < SA_PRESETS; i++)
        [_stopAfterPopup addItemWithObjectValue:@(sa_preset_names[i])];
    _stopAfterPopup.target = self;
    _stopAfterPopup.action = @selector(stopAfterChanged:);
    _stopAfterPopup.delegate = (id)self; /* typing a time and leaving the box counts too */
    limit_size(_stopAfterPopup, 120, 0, 24); /* a combo box has no intrinsic height on 10.9 */
    {
        NSUserDefaults *sud = NSUserDefaults.standardUserDefaults;
        int saved = [sud objectForKey:@"stopAfterMinutes"] ? (int)[sud integerForKey:@"stopAfterMinutes"]
                                                           : sa_from_index((int)[sud integerForKey:@"stopAfter"]);
        char lb[48];
        saved = MAX(0, MIN(SA_MAX, saved));
        sa_label(saved, lb, sizeof(lb));
        _stopAfterPopup.stringValue = @(lb);
        _stopAfterMin = saved;
    }
    _stopAfterPopup.toolTip = @"Stop the recording by itself after this long. Pick a time, or type your own: 45, 2h, 1:30...";

    Box *rowA = stack(@[ [self label:@"Audio"], _audioPopup, [self label:@"Stop after"], _stopAfterPopup,
                         [self label:@"Save to"], _folderButton ]);
    rowA.spacing = 8;
    [rowA setGap:16 afterView:_audioPopup];
    [rowA setGap:16 afterView:_stopAfterPopup];

    Box *rowOpt = stack(@[ _deintCheck, _wideCheck, _previewCheck ]);
    rowOpt.spacing = 16;

    Box *row2 = stack(@[ _recordButton, _timeLabel, meters, _monitorCheck, _statusLabel ]);
    row2.flexible = _statusLabel;
    row2.spacing = 16;

    Box *bar = stack(@[ row1, rowA, rowOpt, row2 ]);
    bar.orientation = NSUserInterfaceLayoutOrientationVertical;
    bar.spacing = 8;
    bar.edgeInsets = NSEdgeInsetsMake(10, 12, 10, 12);
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    _bar = bar;

    NSView *content = [NSView new];
    [content addSubview:_preview];
    [content addSubview:bar];
    activate_all(@[
        mk(_preview, NSLayoutAttributeTop, NSLayoutRelationEqual, content, NSLayoutAttributeTop, 1, 0),
        mk(_preview, NSLayoutAttributeLeading, NSLayoutRelationEqual, content, NSLayoutAttributeLeading, 1, 0),
        mk(_preview, NSLayoutAttributeTrailing, NSLayoutRelationEqual, content, NSLayoutAttributeTrailing, 1, 0),
        mk(bar, NSLayoutAttributeTop, NSLayoutRelationEqual, _preview, NSLayoutAttributeBottom, 1, 0),
        mk(bar, NSLayoutAttributeLeading, NSLayoutRelationEqual, content, NSLayoutAttributeLeading, 1, 0),
        mk(bar, NSLayoutAttributeTrailing, NSLayoutRelationEqual, content, NSLayoutAttributeTrailing, 1, 0),
        mk(bar, NSLayoutAttributeBottom, NSLayoutRelationEqual, content, NSLayoutAttributeBottom, 1, 0),
        mk(_preview, NSLayoutAttributeWidth, NSLayoutRelationGreaterThanOrEqual, nil, 0, 1, 360),
    ]);
    [bar setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical];
    [bar setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                  forOrientation:NSLayoutConstraintOrientationHorizontal];
    [bar setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                  forOrientation:NSLayoutConstraintOrientationVertical];
    _preview.widescreen = _widescreen;
    [self applyAspect];
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
    [appMenu addItemWithTitle:@"Quit DazzleRecorder" action:@selector(terminate:) keyEquivalent:@"q"];
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
    [view addItemWithTitle:@"Show Preview" action:@selector(togglePreview:) keyEquivalent:@"e"];
    viewItem.submenu = view;
    NSApp.mainMenu = bar;
}

- (void)updateTitle
{
    _window.title = [NSString stringWithFormat:@"DazzleRecorder — %@ %@", _inputPopup.titleOfSelectedItem ?: @"",
                                               _pal ? @"PAL 720×576" : @"NTSC 720×480"];
}

- (void)updateFolderButton
{
    _folderButton.title = _folder.lastPathComponent ?: @"Movies";
    [_bar refit];
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

/* the picture is 4:3 or 16:9; the window grows and shrinks around it */
- (void)applyAspect
{
    set_active(_aspectConstraint, NO);
    _aspectConstraint = mk(_preview, NSLayoutAttributeHeight, NSLayoutRelationEqual, _preview, NSLayoutAttributeWidth, _widescreen ? 9.0 / 16 : 3.0 / 4, 0);
    /* not required: on a short screen the window may be shorter than the picture's aspect asks for, and the
     * preview letterboxes (a required one pushed the controls under the Dock) */
    _aspectConstraint.priority = 700;
    set_active(_aspectConstraint, YES);
}

- (void)widescreenChanged:(id)sender
{
    _widescreen = _wideCheck.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:_widescreen forKey:@"widescreen"];
    _preview.widescreen = _widescreen;
    [self applyAspect];
}

- (void)previewChanged:(id)sender
{
    _previewOn = _previewCheck.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:_previewOn forKey:@"preview"];
    _preview.message.hidden = _previewOn;
    if (!_previewOn) {
        [_lastLock lock];
        if (_lastFrame)
            CVPixelBufferRelease(_lastFrame);
        _lastFrame = NULL;
        [_lastLock unlock];
        AVSampleBufferDisplayLayer *layer = _preview.displayLayer;
        if (@available(macOS 14.0, *))
            [layer.sampleBufferRenderer flushWithRemovalOfDisplayedImage:YES completionHandler:nil];
        else
            [layer flushAndRemoveImage];
    }
    [_preview setNeedsLayout:YES];
}

- (void)togglePreview:(id)sender
{
    _previewCheck.state = _previewCheck.state == NSControlStateValueOn ? NSControlStateValueOff : NSControlStateValueOn;
    [self previewChanged:sender];
}

- (void)deinterlaceChanged:(id)sender
{
    _deinterlace = _deintCheck.state == NSControlStateValueOn;
    [NSUserDefaults.standardUserDefaults setBool:_deinterlace forKey:@"deinterlace"];
}

/* the quality steps are bitrates for MPEG-2 and x264 constant-quality levels for H.264 */
static const int kMpegKbps[] = { 6000, 8000, 9500 }, kH264Crf[] = { 23, 20, 17 };

- (void)fillQualityPopup
{
    NSInteger q = MAX(0, MIN(2, [NSUserDefaults.standardUserDefaults integerForKey:@"quality"]));
    [_qualityPopup removeAllItems];
    if (_formatPopup.indexOfSelectedItem == 1)
        [_qualityPopup addItemsWithTitles:@[ @"Standard (CRF 23)", @"High (CRF 20)", @"Maximum (CRF 17)" ]];
    else
        [_qualityPopup addItemsWithTitles:@[ @"Standard (6 Mbit/s)", @"High (8 Mbit/s)", @"Maximum (9.5 Mbit/s)" ]];
    [_qualityPopup selectItemAtIndex:q];
    [_bar refit];
}

- (void)formatChanged:(id)sender
{
    [NSUserDefaults.standardUserDefaults setInteger:_formatPopup.indexOfSelectedItem forKey:@"format"];
    [self fillQualityPopup];
}

- (void)qualityChanged:(id)sender
{
    [NSUserDefaults.standardUserDefaults setInteger:_qualityPopup.indexOfSelectedItem forKey:@"quality"];
}

- (void)audioSourceChanged:(id)sender
{
    int line = _audioPopup.indexOfSelectedItem == 1;
    pthread_mutex_lock(&_recLock);
    _audioOK = 0; /* recordings get silence while the input is switched */
    pthread_mutex_unlock(&_recLock);
    dvc100_audio_stop(_dev);
    sa_set_line_in(line);
    [NSUserDefaults.standardUserDefaults setInteger:line forKey:@"audioSource"];
    [self setStatus:@""];
    [self requestMicrophoneThen:^{ [self startAudio]; }];
}

- (void)monitorChanged:(id)sender
{
    BOOL on = _monitorCheck.state == NSControlStateValueOn;
    g_monitor = on;
    if (@available(macOS 10.15, *))
        ((AVAudioEngine *)_engine).mainMixerNode.outputVolume = on ? 1 : 0;
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
    if (item.action == @selector(togglePreview:))
        item.state = _previewOn ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

/* ---- recording ---- */

- (void)stopAfterChanged:(id)sender
{
    [self applyStopAfter];
}

/* Reads the "Stop after" box (a preset or a typed time), shows it tidily and remembers it. */
- (void)applyStopAfter
{
    int mins = sa_parse(_stopAfterPopup.stringValue.UTF8String);
    if (mins < 0)
        mins = (int)[NSUserDefaults.standardUserDefaults integerForKey:@"stopAfterMinutes"]; /* not a time: keep the last one */
    char lb[48];
    sa_label(mins, lb, sizeof(lb));
    _stopAfterPopup.stringValue = @(lb);
    _stopAfterMin = mins;
    [NSUserDefaults.standardUserDefaults setInteger:mins forKey:@"stopAfterMinutes"];
}

- (void)controlTextDidEndEditing:(NSNotification *)n
{
    if (n.object == _stopAfterPopup)
        [self applyStopAfter];
}

- (void)toggleRecord:(id)sender
{
    if (_rec)
        [self stopRecording];
    else
        [self startRecording];
}

- (void)startRecording
{
    rec_codec_t codec = _formatPopup.indexOfSelectedItem == 1 ? REC_H264 : REC_MPEG2;
    int q = MAX(0, MIN(2, (int)_qualityPopup.indexOfSelectedItem));
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *name = [NSString stringWithFormat:@"DazzleRecorder %@.%s", [df stringFromDate:NSDate.date],
                                                rec_extension(codec)];
    NSString *path = [_folder URLByAppendingPathComponent:name].path;

    recorder_opts_t o = {
        .codec = codec,
        .crf = kH264Crf[q],
        .width = _width,
        .height = _height,
        .pal = _pal,
        .deinterlace = _deinterlace,
        .widescreen = _widescreen,
        .video_kbps = kMpegKbps[q],
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
    pthread_mutex_lock(&_recLock);
    _rec = rec;
    pthread_mutex_unlock(&_recLock);
    _recPath = path;
    _recStart = NSDate.date;
    _recDeinterlaced = o.deinterlace;

    _recordButton.title = @"Stop";

    [_bar refit];
    set_symbol(_recordButton, @"stop.circle.fill", nil);
    _deintCheck.enabled = _wideCheck.enabled = _formatPopup.enabled = _qualityPopup.enabled = _folderButton.enabled =
        _audioPopup.enabled = NO;
    _preview.badge.hidden = NO;
    [self setStatus:[NSString stringWithFormat:@"Recording %@ %@ to %@", o.deinterlace ? @"deinterlaced" : @"interlaced",
                                               codec == REC_H264 ? @"H.264" : @"MPEG-2", name]];
}

- (void)stopRecording
{
    pthread_mutex_lock(&_recLock);
    recorder_t *rec = _rec;
    _rec = NULL;
    pthread_mutex_unlock(&_recLock);
    if (!rec)
        return;

    NSString *path = _recPath;
    _recordButton.enabled = NO;
    _recordButton.title = @"Finishing…";
    [_bar refit];
    [self setStatus:@"Finishing the file…"];
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
        int r = rec_close(rec);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_recordButton.enabled = YES;
            self->_recordButton.title = @"Record";
            [self->_bar refit];
            set_symbol(self->_recordButton, @"record.circle", nil);
            self->_deintCheck.enabled = self->_wideCheck.enabled = self->_formatPopup.enabled =
                self->_qualityPopup.enabled = self->_folderButton.enabled = self->_audioPopup.enabled = YES;
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
        if (_stopAfterMin > 0 && t >= _stopAfterMin * 60.0)
            [self stopRecording]; /* "Stop after" */
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
    if (!pb) {
        [self setStatus:_previewOn ? @"No picture to save yet." : @"Turn the preview on to save a snapshot."];
        return;
    }
    CIImage *img;
    if (@available(macOS 10.11, *)) {
        img = [CIImage imageWithCVPixelBuffer:pb];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        img = [CIImage imageWithCVImageBuffer:pb];
#pragma clang diagnostic pop
    }
    img = [img imageByApplyingTransform:CGAffineTransformMakeScale((_widescreen ? 854.0 : 640.0) / _width, 480.0 / _height)];
    CIContext *ctx = [CIContext context];
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *name = [NSString stringWithFormat:@"DazzleRecorder %@.png", [df stringFromDate:NSDate.date]];
    NSURL *url = [_folder URLByAppendingPathComponent:name];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    NSError *err = nil;
    if (@available(macOS 10.13, *)) {
        [ctx writePNGRepresentationOfImage:img toURL:url format:kCIFormatRGBA8 colorSpace:cs options:@{} error:&err];
    } else {
        CGImageRef cg = [ctx createCGImage:img fromRect:img.extent format:kCIFormatRGBA8 colorSpace:cs];
        NSData *png = nil;
        if (cg) {
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
            png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            CGImageRelease(cg);
        }
        if (!png || ![png writeToURL:url options:NSDataWritingAtomic error:&err])
            if (!err)
                err = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil];
    }
    CGColorSpaceRelease(cs);
    CVPixelBufferRelease(pb);
    [self setStatus:err ? err.localizedDescription : [@"Saved " stringByAppendingString:name]];
}

/* ---- audio ----
 * The DVC100's UAC1 stream is not published by AppleUSBAudio, so it arrives
 * through libusb; the black Dazzle's comes from its Core Audio device. Either
 * way it is played via a ring buffer. */

/* Pull n frames from the ring into planar float buffers, 60 ms behind the input; returns 0 if all silence. */
static volatile int g_monitor = 1; /* AudioUnit path only; AVAudioEngine mutes in its mixer */
static int ring_render(float *L, float *R, uint32_t n, float gain)
{
    return monitor_ring_read(&g_ring, (uint32_t)g_audio_rate * 60 / 1000, NULL, L, R, (int)n, gain);
}

static OSStatus output_unit_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                                   UInt32 bus, UInt32 n, AudioBufferList *abl)
{
    (void)ref, (void)ts, (void)bus;
    if (!ring_render(abl->mBuffers[0].mData, abl->mBuffers[1].mData, n, g_monitor ? 1.0f : 0.0f))
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

/* macOS 10.14+ gates the Dazzle's sound input behind the Microphone privacy setting. Ask for it up front
 * (the system shows its prompt once) and say how to fix it if it was refused. */
- (void)requestMicrophoneThen:(void (^)(void))go
{
    if (@available(macOS 10.14, *)) {
        AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (st == AVAuthorizationStatusNotDetermined) {
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL ok) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (ok) go(); else [self microphoneDenied];
                });
            }];
            return;
        }
        if (st != AVAuthorizationStatusAuthorized) {
            [self microphoneDenied];
            return;
        }
    }
    go();
}

- (void)microphoneDenied
{
    [self setStatus:@"No sound: microphone access is off for DazzleRecorder (System Settings > Privacy & Security > Microphone)"];
    NSAlert *a = [NSAlert new];
    a.messageText = @"DazzleRecorder needs microphone access";
    a.informativeText = @"macOS treats the Dazzle's audio input as a microphone. Turn on DazzleRecorder under "
                        @"Privacy & Security > Microphone, then restart the app, or you will record without sound.";
    [a addButtonWithTitle:@"Open Settings"];
    [a addButtonWithTitle:@"Continue Without Sound"];
    if ([a runModal] == NSAlertFirstButtonReturn)
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:
            @"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"]];
}

- (void)startAudio
{
    int r = dvc100_audio_start(_dev, audio_cb, (__bridge void *)self, &g_audio_rate);
    if (r) {
        const char *why = sa_last_error();
        [self setStatus:[NSString stringWithFormat:@"Audio unavailable: %s", why ? why : dvc100_strerror(r)]];
        return;
    }
    pthread_mutex_lock(&_recLock);
    _audioOK = 1;
    pthread_mutex_unlock(&_recLock);
    g_monitor = _monitorCheck.state == NSControlStateValueOn;
    monitor_ring_restart(&g_ring);
    if (_engine || _outputUnit)
        return; /* switching the source: the monitor output is already running */
    if (@available(macOS 10.15, *)) {
        _engine = [AVAudioEngine new];
        AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:g_audio_rate channels:2];
        AVAudioSourceNode *src = [[AVAudioSourceNode alloc]
            initWithFormat:fmt
               renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount n,
                                     AudioBufferList *abl) {
                   (void)ts;
                   *silence = !ring_render(abl->mBuffers[0].mData, abl->mBuffers[1].mData, n, 1.0f);
                   return noErr;
               }];
        [_engine attachNode:src];
        NSError *err = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" /* the error: variant needs macOS 27 */
        [_engine connect:src to:((AVAudioEngine *)_engine).mainMixerNode format:fmt];
#pragma clang diagnostic pop
        ((AVAudioEngine *)_engine).mainMixerNode.outputVolume = g_monitor ? 1 : 0;
        if (![_engine startAndReturnError:&err])
            [self setStatus:[@"Audio output: " stringByAppendingString:err.localizedDescription]];
        return;
    }

    /* macOS 10.11-10.14: the default output unit, which converts the rate itself */
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = au_find_next(NULL, &desc);
    OSStatus st = comp ? au_instance_new(comp, &_outputUnit) : -1;
    if (st == noErr) {
        AudioStreamBasicDescription f = {
            .mSampleRate = g_audio_rate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
            .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4,
            .mChannelsPerFrame = 2, .mBitsPerChannel = 32,
        };
        AURenderCallbackStruct cb = { output_unit_render, NULL };
        st = au_set_property(_outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f,
                                  sizeof(f));
        if (st == noErr)
            st = au_set_property(_outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                      0, &cb, sizeof(cb));
        if (st == noErr)
            st = au_initialize(_outputUnit);
        if (st == noErr)
            st = au_output_start(_outputUnit);
    }
    if (st != noErr) {
        if (_outputUnit)
            au_instance_dispose(_outputUnit);
        _outputUnit = NULL;
        [self setStatus:[NSString stringWithFormat:@"Audio output failed (error %d)", (int)st]];
    }
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
    pthread_mutex_lock(&_recLock);
    recorder_t *rec = _rec;
    _rec = NULL;
    pthread_mutex_unlock(&_recLock);
    rec_close(rec);
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)applicationWillTerminate:(NSNotification *)note
{
    [_uiTimer invalidate];
    if (@available(macOS 10.15, *))
        [(AVAudioEngine *)_engine stop];
    if (_outputUnit) {
        au_output_stop(_outputUnit);
        au_instance_dispose(_outputUnit);
        _outputUnit = NULL;
    }
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
