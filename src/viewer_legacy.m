/*
 * DazzleRecorder for old Macs: Mac OS X 10.3 on PowerPC and 10.4 on Intel, up
 * to 10.8 (10.9 and later run the regular app, viewer.m).
 *
 * It is the same recorder (app_core.c does the device, sound, recording and
 * snapshots) under a Cocoa window written for the oldest AppKit: manual
 * retain/release, no blocks, no Auto Layout, no layers. The preview is an
 * OpenGL view that takes the Dazzle's YUYV frames as they come and lets the
 * graphics card convert them to RGB (GL_APPLE_ycbcr_422, which every Mac with
 * Quartz Extreme has), so a G4 is not spending its time on colour maths.
 *
 * Recording formats: on PowerPC, DV (the default: iMovie's own format and
 * cheap to encode) or MPEG-2 with a lighter encoder setting; H.264 is left out
 * because x264 cannot keep up with live video on a G4 or G5. On Intel,
 * MPEG-2 (the default), H.264 or DV.
 */
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include <string.h>

#include "app_core.h"
#include "recorder.h"

#ifndef GL_YCBCR_422_APPLE
#define GL_YCBCR_422_APPLE 0x85B9
#endif
#ifndef GL_UNSIGNED_SHORT_8_8_APPLE
#define GL_UNSIGNED_SHORT_8_8_APPLE 0x85BA
#define GL_UNSIGNED_SHORT_8_8_REV_APPLE 0x85BB
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

/* Y0 Cb Y1 Cr bytes: the packed 16-bit type whose first byte in memory is luma */
#ifdef __BIG_ENDIAN__
#define YUYV_TYPE GL_UNSIGNED_SHORT_8_8_REV_APPLE
#else
#define YUYV_TYPE GL_UNSIGNED_SHORT_8_8_APPLE
#endif

#define TEX_SIZE 1024 /* power of two, holds 720x576 */

static app_t *g_app;

/* recording formats offered on this CPU, the first is the default */
#ifdef __ppc__
static const int kFormats[] = { REC_DV, REC_MPEG2 };
#else
static const int kFormats[] = { REC_MPEG2, REC_H264, REC_DV };
#endif
#define NFORMATS ((int)(sizeof(kFormats) / sizeof(kFormats[0])))

static NSString *format_name(int codec)
{
    return codec == REC_DV ? @"DV" : codec == REC_H264 ? @"H.264" : @"MPEG-2";
}

/* ---- preview ---- */

@interface PreviewView : NSOpenGLView {
    int _w, _h;
    uint8_t *_buf;
    uint64_t _seq;
    GLuint _tex;
    BOOL _glReady, _ycbcr, _wide, _haveFrame;
}
- (void)setFrameWidth:(int)w height:(int)h;
- (void)setWidescreen:(BOOL)on;
- (void)poll;
- (void)clear;
- (void)drawPicture;
@end

@implementation PreviewView
- (id)initWithFrame:(NSRect)frame
{
    NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLPixelFormat *pf = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs] autorelease];
    if ((self = [super initWithFrame:frame pixelFormat:pf])) {
        _w = 720;
        _h = 480;
    }
    return self;
}

- (void)dealloc
{
    free(_buf);
    [super dealloc];
}

- (void)setFrameWidth:(int)w height:(int)h
{
    _w = w;
    _h = h;
    free(_buf);
    _buf = calloc((size_t)w * h, 4);
}

- (void)setWidescreen:(BOOL)on
{
    _wide = on;
    [self setNeedsDisplay:YES];
}

- (BOOL)isOpaque { return YES; }

- (void)setupGL
{
    [[self openGLContext] makeCurrentContext];
    GLint one = 1; /* wait for the screen's refresh when swapping, so frames are not torn */
    [[self openGLContext] setValues:&one forParameter:NSOpenGLCPSwapInterval];
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    _ycbcr = ext && strstr(ext, "GL_APPLE_ycbcr_422") != NULL;
    app_set_preview_yuyv(g_app, _ycbcr); /* otherwise app_core converts to RGB itself */
    _seq = 0;
    glGenTextures(1, &_tex);
    glBindTexture(GL_TEXTURE_2D, _tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, TEX_SIZE, TEX_SIZE, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    _glReady = YES;
}

/* takes a new frame, if there is one, onto the texture and draws */
- (void)poll
{
    if (!g_app || !_buf || ![self window] || ![[self window] isVisible])
        return;
    if (!_glReady)
        [self setupGL];
    [[self openGLContext] makeCurrentContext];
    int got = _ycbcr ? app_get_frame_yuyv(g_app, _buf, &_seq) : app_get_frame(g_app, (uint32_t *)(void *)_buf, &_seq);
    if (!got)
        return;
    glBindTexture(GL_TEXTURE_2D, _tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, _w);
    if (_ycbcr)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _w, _h, GL_YCBCR_422_APPLE, YUYV_TYPE, _buf);
    else
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _w, _h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, _buf);
    _haveFrame = YES;
    [self drawPicture];
}

- (void)clear
{
    _haveFrame = NO;
    if (!_glReady)
        return;
    [[self openGLContext] makeCurrentContext];
    [self drawPicture];
}

- (void)drawPicture
{
    NSRect b = [self bounds];
    float W = (float)b.size.width, H = (float)b.size.height;
    glViewport(0, 0, (GLsizei)W, (GLsizei)H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (_haveFrame && W > 0 && H > 0) {
        /* 720 BT.601 samples span a 4:3 picture: fit a 4:3 (or 16:9) rectangle */
        float ar = _wide ? 16.0f / 9 : 4.0f / 3, w = W, h = W / ar;
        if (h > H) {
            h = H;
            w = H * ar;
        }
        float x0 = -w / W, x1 = w / W, y0 = -h / H, y1 = h / H;
        float u = (float)_w / TEX_SIZE, v = (float)_h / TEX_SIZE;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, _tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBegin(GL_QUADS);
        glTexCoord2f(0, v); glVertex2f(x0, y0);
        glTexCoord2f(u, v); glVertex2f(x1, y0);
        glTexCoord2f(u, 0); glVertex2f(x1, y1);
        glTexCoord2f(0, 0); glVertex2f(x0, y1);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }
    [[self openGLContext] flushBuffer];
}

- (void)drawRect:(NSRect)r
{
    (void)r;
    if (!_glReady && g_app)
        [self setupGL];
    [[self openGLContext] makeCurrentContext];
    [self drawPicture];
}
@end

/* ---- level meters (NSLevelIndicator needs 10.4) ---- */

@interface MeterView : NSView {
    float _v[2]; /* 0..1 */
}
- (void)setLeft:(float)l right:(float)r;
@end

@implementation MeterView
- (void)setLeft:(float)l right:(float)r
{
    if (l == _v[0] && r == _v[1])
        return;
    _v[0] = l;
    _v[1] = r;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect
{
    (void)rect;
    NSRect b = [self bounds];
    float bar = (float)floor((b.size.height - 3) / 2);
    int c;
    for (c = 0; c < 2; c++) {
        NSRect r = NSMakeRect(0, c ? 0 : bar + 3, b.size.width, bar);
        [[NSColor colorWithCalibratedWhite:0.2f alpha:1] set];
        NSRectFill(r);
        float v = _v[c];
        NSRect f = r;
        f.size.width = (float)floor(r.size.width * (v < 0.8f ? v : 0.8f));
        [[NSColor colorWithCalibratedRed:0.2f green:0.8f blue:0.2f alpha:1] set];
        NSRectFill(f);
        if (v > 0.8f) {
            f.origin.x = r.size.width * 0.8f;
            f.size.width = (float)floor(r.size.width * ((v < 0.95f ? v : 0.95f) - 0.8f));
            [[NSColor colorWithCalibratedRed:0.95f green:0.8f blue:0.1f alpha:1] set];
            NSRectFill(f);
        }
        if (v > 0.95f) {
            f.origin.x = r.size.width * 0.95f;
            f.size.width = (float)floor(r.size.width * (v - 0.95f));
            [[NSColor colorWithCalibratedRed:0.9f green:0.15f blue:0.1f alpha:1] set];
            NSRectFill(f);
        }
    }
}
@end

/* ---- the window ---- */

@interface Controller : NSObject {
    NSWindow *_window;
    PreviewView *_preview;
    NSView *_bar;
    NSPopUpButton *_input, *_format, *_quality, *_audio;
    NSButton *_deint, *_wide, *_previewCheck, *_monitor, *_record, *_folderButton;
    NSTextField *_time, *_status;
    MeterView *_meters;
    NSTimer *_timer;
    NSString *_folder;
    BOOL _finishing;
    int _ticks;
    double _level[2];
}
- (NSTextField *)label:(NSString *)s;
@end

static NSTextField *make_label(NSString *s)
{
    NSTextField *l = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 10, 17)] autorelease];
    [l setStringValue:s];
    [l setBezeled:NO];
    [l setDrawsBackground:NO];
    [l setEditable:NO];
    [l setSelectable:NO];
    [l setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [l sizeToFit];
    return l;
}

static NSButton *make_button(NSString *title, id target, SEL action, BOOL checkbox)
{
    NSButton *b = [[[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 10, 24)] autorelease];
    [b setButtonType:checkbox ? NSSwitchButton : NSMomentaryPushInButton];
    if (!checkbox)
        [b setBezelStyle:NSRoundedBezelStyle];
    [b setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [b setTitle:title];
    [b setTarget:target];
    [b setAction:action];
    [b sizeToFit];
    return b;
}

static NSPopUpButton *make_popup(NSArray *items, id target, SEL action)
{
    NSPopUpButton *p = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 10, 26) pullsDown:NO] autorelease];
    [p setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [p addItemsWithTitles:items];
    [p setTarget:target];
    [p setAction:action];
    [p sizeToFit];
    return p;
}

static void rec_done(void *user, int ok, const char *path);

@implementation Controller

- (NSUserDefaults *)ud { return [NSUserDefaults standardUserDefaults]; }

- (int)selectedCodec
{
    int i = (int)[_format indexOfSelectedItem];
    return kFormats[i < 0 || i >= NFORMATS ? 0 : i];
}

- (void)fillQuality
{
    int codec = [self selectedCodec], q = (int)[[self ud] integerForKey:@"quality"];
    [_quality removeAllItems];
    if (codec == REC_DV) {
        [_quality addItemWithTitle:@"25 Mbit/s (DV)"];
    } else if (codec == REC_H264) {
        [_quality addItemsWithTitles:[NSArray arrayWithObjects:@"Standard (CRF 23)", @"High (CRF 20)",
                                                               @"Maximum (CRF 17)", nil]];
    } else {
        [_quality addItemsWithTitles:[NSArray arrayWithObjects:@"Standard (6 Mbit/s)", @"High (8 Mbit/s)",
                                                               @"Maximum (9.5 Mbit/s)", nil]];
    }
    [_quality selectItemAtIndex:codec == REC_DV ? 0 : q < 0 ? 0 : q > 2 ? 2 : q];
    [_quality setEnabled:codec != REC_DV && !app_is_recording(g_app)];
}

/* sizes a control to its content, with a few points spare (text measured exactly can lose its last letter) */
static void fit(NSView *v, float minw, float maxw)
{
    if ([v respondsToSelector:@selector(sizeToFit)])
        [(NSControl *)v sizeToFit];
    NSRect f = [v frame];
    f.size.width = (float)ceil(f.size.width) + 4;
    if (f.size.width < minw)
        f.size.width = minw;
    if (maxw > 0 && f.size.width > maxw)
        f.size.width = maxw;
    [v setFrame:f];
}

/* places views left to right, vertically centred on a row, from x; returns where it ended */
static float place_row(NSArray *views, float x, float y, float h, float gap)
{
    unsigned i;
    for (i = 0; i < [views count]; i++) {
        NSView *v = [views objectAtIndex:i];
        if ([v isKindOfClass:[NSNumber class]]) { /* extra space */
            x += [(NSNumber *)v floatValue];
            continue;
        }
        NSRect f = [v frame];
        f.origin.x = x;
        f.origin.y = (float)floor(y + (h - f.size.height) / 2);
        [v setFrame:f];
        x += f.size.width + gap;
    }
    return x;
}

- (void)layoutBar
{
    NSRect b = [_bar bounds];
    NSArray *row1 = [NSArray arrayWithObjects:[self label:@"Input"], _input, [NSNumber numberWithFloat:8], _deint, _wide, _previewCheck,
                                              [NSNumber numberWithFloat:8], [self label:@"Format"], _format,
                                              [self label:@"Quality"], _quality, [NSNumber numberWithFloat:8],
                                              [self label:@"Save to"], _folderButton, nil];
    NSArray *row2 = [NSArray arrayWithObjects:_record, _time, _meters, _monitor, [self label:@"Audio"], _audio, nil];
    unsigned i;
    for (i = 0; i < [row1 count]; i++) {
        id v = [row1 objectAtIndex:i];
        if (v != _folderButton && ![v isKindOfClass:[NSNumber class]])
            fit(v, 0, 0);
    }
    fit(_folderButton, 60, 200);
    fit(_record, 100, 0);
    fit(_time, 0, 0);
    fit(_monitor, 0, 0);
    fit([self label:@"Audio"], 0, 0);
    fit(_audio, 0, 0);
    place_row(row1, 12, 44, 26, 6);
    float x = place_row(row2, 12, 8, 28, 12);
    NSRect sf = [_status frame];
    sf.origin.x = x;
    sf.origin.y = 13;
    sf.size.width = b.size.width - x - 12;
    [_status setFrame:sf];
}

/* the row labels are made once and kept in the bar */
- (NSTextField *)label:(NSString *)s
{
    NSArray *subs = [_bar subviews];
    unsigned i;
    for (i = 0; i < [subs count]; i++) {
        id v = [subs objectAtIndex:i];
        if ([v isKindOfClass:[NSTextField class]] && v != _time && v != _status && [[v stringValue] isEqualToString:s])
            return v;
    }
    NSTextField *l = make_label(s);
    [_bar addSubview:l];
    return l;
}

- (void)buildMenu
{
    NSMenu *bar = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSMenuItem *appItem = [[[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@"DazzleRecorder"] autorelease];
    [appMenu addItemWithTitle:@"Hide DazzleRecorder" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit DazzleRecorder" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *fileItem = [[[NSMenuItem alloc] initWithTitle:@"File" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *file = [[[NSMenu alloc] initWithTitle:@"File"] autorelease];
    [[file addItemWithTitle:@"Start/Stop Recording" action:@selector(toggleRecord:) keyEquivalent:@"r"] setTarget:self];
    [[file addItemWithTitle:@"Save Snapshot" action:@selector(snapshot:) keyEquivalent:@"s"] setTarget:self];
    [file addItem:[NSMenuItem separatorItem]];
    [[file addItemWithTitle:@"Choose Recording Folder..." action:@selector(chooseFolder:) keyEquivalent:@""]
        setTarget:self];
    [[file addItemWithTitle:@"Show Recordings in Finder" action:@selector(revealFolder:) keyEquivalent:@""]
        setTarget:self];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [fileItem setSubmenu:file];
    [bar addItem:fileItem];

    [NSApp setMainMenu:bar];
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) /* 10.3 needs it to treat the first menu as the app menu */
        [NSApp performSelector:@selector(setAppleMenu:) withObject:appMenu];
}

- (void)updateTitle
{
    [_window setTitle:[NSString stringWithFormat:@"DazzleRecorder - %@ %@", [_input titleOfSelectedItem],
                                                 app_is_pal(g_app) ? @"PAL 720x576" : @"NTSC 720x480"]];
}

- (void)updateFolderButton
{
    [_folderButton setTitle:[[NSFileManager defaultManager] displayNameAtPath:_folder]];
    [_folderButton setToolTip:_folder];
    [self layoutBar];
}

- (void)setStatus:(NSString *)s
{
    [_status setStringValue:s ? s : @""];
}

- (void)buildWindow
{
    NSUserDefaults *ud = [self ud];
    int barH = 78;
    /* short screens: keep the controls clear of the Dock */
    float maxH = (float)[[NSScreen mainScreen] visibleFrame].size.height - 60;
    NSRect frame = NSMakeRect(0, 0, 800, 600 + barH > maxH ? maxH : 600 + barH);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setDelegate:self];
    [_window setMinSize:NSMakeSize(860, 300 + barH)];
    NSView *content = [_window contentView];

    _preview = [[[PreviewView alloc] initWithFrame:NSMakeRect(0, barH, 800, 600)] autorelease];
    [_preview setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_preview setFrameWidth:app_width(g_app) height:app_height(g_app)];
    [_preview setWidescreen:[ud boolForKey:@"widescreen"]];
    [content addSubview:_preview];

    _bar = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, barH)] autorelease];
    [_bar setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:_bar];

    _input = make_popup([NSArray arrayWithObjects:@"Composite", @"S-Video", nil], self, @selector(inputChanged:));
    [_input selectItemAtIndex:[ud integerForKey:@"input"] == 1 ? 1 : 0];
    _deint = make_button(@"Deinterlace", self, @selector(deinterlaceChanged:), YES);
    [_deint setState:[ud boolForKey:@"deinterlace"] ? NSOnState : NSOffState];
    [_deint setToolTip:@"Preview and recording. Off records interlaced video, as on a DVD."];
    _wide = make_button(@"Widescreen", self, @selector(widescreenChanged:), YES);
    [_wide setState:[ud boolForKey:@"widescreen"] ? NSOnState : NSOffState];
    [_wide setToolTip:@"Stretch the picture to 16:9 in the preview, snapshots and recordings."];

    NSMutableArray *names = [NSMutableArray array];
    int i, fmt = (int)[ud integerForKey:@"format"], sel = 0;
    for (i = 0; i < NFORMATS; i++) {
        [names addObject:format_name(kFormats[i])];
        if (kFormats[i] == fmt)
            sel = i;
    }
    _format = make_popup(names, self, @selector(formatChanged:));
    [_format selectItemAtIndex:sel];
#ifdef __ppc__
    [_format setToolTip:@"DV: iMovie's format, light on the processor (about 13 GB an hour). "
                        @"MPEG-2: a DVD-compatible .mpg, needs a fast G4 or a G5."];
#else
    [_format setToolTip:@"MPEG-2: a DVD-compatible .mpg. H.264: a much smaller .mp4. DV: iMovie's format."];
#endif
    _quality = make_popup([NSArray arrayWithObject:@""], self, @selector(qualityChanged:));
    [self fillQuality];
    _folderButton = make_button(@"Movies", self, @selector(chooseFolder:), NO);

    _record = make_button(@"Record", self, @selector(toggleRecord:), NO);
    [_record setKeyEquivalent:@"r"];
    [_record setKeyEquivalentModifierMask:NSCommandKeyMask];
    _time = make_label(@"00:00:00");
    [_time setFont:[NSFont userFixedPitchFontOfSize:14]];
    [_time sizeToFit];
    _meters = [[[MeterView alloc] initWithFrame:NSMakeRect(0, 0, 90, 15)] autorelease];
    _monitor = make_button(@"Play audio", self, @selector(monitorChanged:), YES);
    [_monitor setState:[ud boolForKey:@"monitor"] ? NSOnState : NSOffState];
    _audio = make_popup([NSArray arrayWithObjects:@"Dazzle", @"Line In", nil], self, @selector(audioChanged:));
    [_audio selectItemAtIndex:[ud integerForKey:@"audioSource"] == 1 ? 1 : 0];
    [_audio setToolTip:@"Record and play the sound from the Dazzle, or from this computer's Line In instead."];
    _previewCheck = make_button(@"Preview", self, @selector(previewChanged:), YES);
    [_previewCheck setState:[ud boolForKey:@"preview"] ? NSOnState : NSOffState];
    [_previewCheck setToolTip:@"Show the picture. Off saves the computer the work of drawing it; recording and sound go on."];
    _status = make_label(@"");
    [_status setTextColor:[NSColor disabledControlTextColor]];
    [[_status cell] setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [_status setAutoresizingMask:NSViewWidthSizable];

    NSArray *all = [NSArray arrayWithObjects:_input, _deint, _wide, _previewCheck, _format, _quality, _folderButton, _record, _time,
                                             _meters, _monitor, _audio, _status, nil];
    for (i = 0; i < (int)[all count]; i++)
        [_bar addSubview:[all objectAtIndex:i]];
    [self updateFolderButton];
    [self updateTitle];
    [_window setFrameAutosaveName:@"DazzleRecorderWindow"];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    NSUserDefaults *ud = [self ud];
    NSDictionary *defaults = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithInt:0], @"input", [NSNumber numberWithBool:YES], @"deinterlace",
        [NSNumber numberWithBool:NO], @"widescreen", [NSNumber numberWithBool:YES], @"preview", [NSNumber numberWithInt:kFormats[0]], @"format",
        [NSNumber numberWithInt:1], @"quality", [NSNumber numberWithBool:YES], @"monitor",
        [NSNumber numberWithInt:0], @"audioSource", nil];
    [ud registerDefaults:defaults];

    /* Cocoa must know threads exist before app_core's threads call back into it */
    [NSThread detachNewThreadSelector:@selector(noop:) toTarget:self withObject:nil];

    char folder[1024];
    app_default_folder(folder, sizeof(folder));
    NSString *saved = [ud stringForKey:@"folder"];
    _folder = [(saved ? saved : [NSString stringWithUTF8String:folder]) retain];

    dvc100_input_t input = [ud integerForKey:@"input"] == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE;
    dvc100_norm_t norm = DVC100_NORM_AUTO;
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    if ([args containsObject:@"--pal"]) norm = DVC100_NORM_PAL;
    if ([args containsObject:@"--ntsc"]) norm = DVC100_NORM_NTSC;
    if ([args containsObject:@"-v"]) dvc100_set_verbose(1);
    app_set_setting_int("line_in", (int)[ud integerForKey:@"audioSource"] == 1);

    [self buildMenu];
    char err[512] = "";
    g_app = app_open(input, norm, err, sizeof(err));
    if (!g_app) {
        [NSApp activateIgnoringOtherApps:YES];
        NSRunAlertPanel(@"No Dazzle capture device available", @"%@", @"Quit", nil, nil,
                        [NSString stringWithUTF8String:err]);
        [NSApp terminate:nil];
        return;
    }
    app_set_deinterlace(g_app, [ud boolForKey:@"deinterlace"]);
    app_set_widescreen(g_app, [ud boolForKey:@"widescreen"]);
    app_set_preview(g_app, [ud boolForKey:@"preview"]);
    app_set_monitor(g_app, [ud boolForKey:@"monitor"]);
    [self buildWindow];
    const char *problem = app_audio_problem(g_app);
    if (problem)
        [self setStatus:[NSString stringWithUTF8String:problem]];
    [NSApp activateIgnoringOtherApps:YES];

    _timer = [[NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil
                                              repeats:YES] retain];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSModalPanelRunLoopMode];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSEventTrackingRunLoopMode];
}

- (void)noop:(id)arg
{
    (void)arg;
}

- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    [self layoutBar];
}

/* ---- controls ---- */

- (void)inputChanged:(id)sender
{
    (void)sender;
    int s = (int)[_input indexOfSelectedItem];
    app_set_input(g_app, s == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE);
    [[self ud] setInteger:s forKey:@"input"];
    [self updateTitle];
}

- (void)deinterlaceChanged:(id)sender
{
    (void)sender;
    BOOL on = [_deint state] == NSOnState;
    app_set_deinterlace(g_app, on);
    [[self ud] setBool:on forKey:@"deinterlace"];
}

- (void)previewChanged:(id)sender
{
    (void)sender;
    BOOL on = [_previewCheck state] == NSOnState;
    app_set_preview(g_app, on);
    [[self ud] setBool:on forKey:@"preview"];
    if (!on)
        [_preview clear];
}

- (void)widescreenChanged:(id)sender
{
    (void)sender;
    BOOL on = [_wide state] == NSOnState;
    app_set_widescreen(g_app, on);
    [_preview setWidescreen:on];
    [[self ud] setBool:on forKey:@"widescreen"];
}

- (void)formatChanged:(id)sender
{
    (void)sender;
    [[self ud] setInteger:[self selectedCodec] forKey:@"format"];
    [self fillQuality];
    [self layoutBar];
}

- (void)qualityChanged:(id)sender
{
    (void)sender;
    if ([self selectedCodec] != REC_DV)
        [[self ud] setInteger:[_quality indexOfSelectedItem] forKey:@"quality"];
}

- (void)monitorChanged:(id)sender
{
    (void)sender;
    BOOL on = [_monitor state] == NSOnState;
    app_set_monitor(g_app, on);
    [[self ud] setBool:on forKey:@"monitor"];
}

- (void)audioChanged:(id)sender
{
    (void)sender;
    int line = [_audio indexOfSelectedItem] == 1;
    [[self ud] setInteger:line forKey:@"audioSource"];
    app_set_audio_source(g_app, line);
    const char *problem = app_audio_problem(g_app);
    [self setStatus:problem ? [NSString stringWithUTF8String:problem] : @""];
}

- (void)chooseFolder:(id)sender
{
    (void)sender;
    if (app_is_recording(g_app))
        return;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setCanChooseDirectories:YES];
    [p setCanChooseFiles:NO];
    [p setPrompt:@"Choose"];
    if ([p respondsToSelector:@selector(setCanCreateDirectories:)])
        [p setCanCreateDirectories:YES];
    if ([p runModalForDirectory:_folder file:nil types:nil] != NSOKButton || [[p filenames] count] == 0)
        return;
    [_folder release];
    _folder = [[[p filenames] objectAtIndex:0] retain];
    [[self ud] setObject:_folder forKey:@"folder"];
    [self updateFolderButton];
}

- (void)revealFolder:(id)sender
{
    (void)sender;
    [[NSWorkspace sharedWorkspace] openFile:_folder];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if ([item action] == @selector(toggleRecord:))
        [item setTitle:app_is_recording(g_app) ? @"Stop Recording" : @"Start Recording"];
    if ([item action] == @selector(toggleRecord:) || [item action] == @selector(snapshot:))
        return g_app && !_finishing;
    if ([item action] == @selector(chooseFolder:))
        return g_app && !app_is_recording(g_app);
    return YES;
}

- (void)setControlsEnabled:(BOOL)on
{
    [_deint setEnabled:on];
    [_wide setEnabled:on];
    [_format setEnabled:on];
    [_folderButton setEnabled:on];
    [_audio setEnabled:on];
    [_quality setEnabled:on && [self selectedCodec] != REC_DV];
}

/* ---- recording ---- */

- (void)toggleRecord:(id)sender
{
    (void)sender;
    if (_finishing)
        return;
    if (app_is_recording(g_app)) {
        _finishing = YES;
        [_record setEnabled:NO];
        [_record setTitle:@"Finishing..."];
        [self setStatus:@"Finishing the file..."];
        app_record_stop(g_app, rec_done, self);
        return;
    }
    char path[1024], err[512] = "";
    int codec = [self selectedCodec];
    if (app_record_start_codec(g_app, [_folder fileSystemRepresentation], codec, (int)[_quality indexOfSelectedItem],
                               [_deint state] == NSOnState, path, sizeof(path), err, sizeof(err))) {
        NSRunAlertPanel(@"Cannot start recording", @"%@", @"OK", nil, nil, [NSString stringWithUTF8String:err]);
        return;
    }
    [_record setTitle:@"Stop"];
    [self setControlsEnabled:NO];
    [self setStatus:[NSString stringWithFormat:@"Recording %@ to %@", format_name(codec),
                                               [[NSString stringWithUTF8String:path] lastPathComponent]]];
    [self layoutBar];
}

- (void)recordingDone:(NSDictionary *)info
{
    _finishing = NO;
    [_record setEnabled:YES];
    [_record setTitle:@"Record"];
    [self setControlsEnabled:YES];
    NSString *path = [info objectForKey:@"path"];
    if ([[info objectForKey:@"ok"] boolValue]) {
        [self setStatus:[@"Saved " stringByAppendingString:[path lastPathComponent]]];
        [[NSWorkspace sharedWorkspace] selectFile:path inFileViewerRootedAtPath:@""];
    } else {
        [self setStatus:@"Recording failed (ffmpeg error), see Console for details"];
    }
    [self layoutBar];
}

static void rec_done(void *user, int ok, const char *path)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:[NSNumber numberWithBool:ok != 0], @"ok",
                                                                    [NSString stringWithUTF8String:path], @"path", nil];
    [(Controller *)user performSelectorOnMainThread:@selector(recordingDone:) withObject:info waitUntilDone:NO];
    [pool release];
}

- (void)snapshot:(id)sender
{
    (void)sender;
    char path[1024], err[512] = "";
    if (app_snapshot(g_app, [_folder fileSystemRepresentation], path, sizeof(path), err, sizeof(err)))
        [self setStatus:[NSString stringWithUTF8String:err]];
    else
        [self setStatus:[@"Saved " stringByAppendingString:[[NSString stringWithUTF8String:path] lastPathComponent]]];
}

/* ---- timer: the picture every refresh, meters and clock a few times a second ---- */

- (void)tick:(NSTimer *)t
{
    (void)t;
    if (!g_app)
        return;
    [_preview poll];
    if (++_ticks % 3)
        return;
    double db[2];
    app_peaks(g_app, db);
    int c;
    for (c = 0; c < 2; c++) {
        double v = db[c] + 60; /* -60..0 dBFS -> 0..60 */
        _level[c] = v > _level[c] - 1.5 ? v : _level[c] - 1.5;
    }
    [_meters setLeft:(float)(_level[0] / 60) right:(float)(_level[1] / 60)];
    int s = (int)app_record_seconds(g_app);
    [_time setStringValue:[NSString stringWithFormat:@"%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60]];
}

/* ---- quitting ---- */

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    if (!g_app || !app_is_recording(g_app))
        return NSTerminateNow;
    if (NSRunAlertPanel(@"A recording is in progress", @"Stop the recording and quit?", @"Stop and Quit", @"Cancel",
                        nil) != NSAlertDefaultReturn)
        return NSTerminateCancel;
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_timer invalidate];
    app_close(g_app); /* finishes a recording still in progress */
    g_app = NULL;
}
@end

int main(int argc, const char **argv)
{
    (void)argc, (void)argv;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    Controller *c = [[Controller alloc] init];
    [NSApp setDelegate:c];
    [NSApp run];
    [pool release];
    return 0;
}
