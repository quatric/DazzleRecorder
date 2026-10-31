/*
 * Dazzle Capture for Linux: a plain Xlib window with the live preview, level
 * meters and the same controls as the Windows and macOS apps. libX11 is
 * loaded at run time, so it builds without any X headers or libraries and
 * runs on X11 and under Wayland through XWayland. The Xlib types used here
 * are declared by hand; they follow the Xlib ABI (long-sized fields).
 */
#define _GNU_SOURCE
#include "app_core.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- minimal Xlib ---- */

typedef struct _XDisplay Display;
typedef unsigned long XID, Window, Atom, Pixmap, Font;
typedef struct _XGC *GC;

typedef struct { int type; unsigned long serial; int send_event; Display *display; Window window; } XAnyEvent;
typedef struct {
    int type; unsigned long serial; int send_event; Display *display; Window window, root, subwindow;
    unsigned long time; int x, y, x_root, y_root; unsigned int state, keycode; int same_screen;
} XKeyEvent;
typedef XKeyEvent XButtonEvent; /* keycode is the button number there */
typedef struct {
    int type; unsigned long serial; int send_event; Display *display; Window event, window;
    int x, y, width, height, border_width; Window above; int override_redirect;
} XConfigureEvent;
typedef struct {
    int type; unsigned long serial; int send_event; Display *display; Window window; Atom message_type;
    int format; union { char b[20]; short s[10]; long l[5]; } data;
} XClientMessageEvent;
typedef union {
    int type; XAnyEvent xany; XKeyEvent xkey; XButtonEvent xbutton; XConfigureEvent xconfigure;
    XClientMessageEvent xclient; long pad[24];
} XEvent;
typedef struct { int width, height, xoffset, format; char *data; } XImageHead;

enum { KeyPress = 2, ButtonPress = 4, Expose = 12, ConfigureNotify = 22, ClientMessage = 33, MapNotify = 19 };
enum { KeyPressMask = 1, ButtonPressMask = 4, ExposureMask = 1 << 15, StructureNotifyMask = 1 << 17 };
enum { SubstructureNotifyMask = 1 << 19, SubstructureRedirectMask = 1 << 20 };

#define X_FUNCS(X) \
    X(Display *, XOpenDisplay, (const char *)) \
    X(int, XCloseDisplay, (Display *)) \
    X(int, XDefaultScreen, (Display *)) \
    X(Window, XRootWindow, (Display *, int)) \
    X(int, XDefaultDepth, (Display *, int)) \
    X(void *, XDefaultVisual, (Display *, int)) \
    X(Window, XCreateSimpleWindow, (Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long)) \
    X(int, XMapWindow, (Display *, Window)) \
    X(int, XSelectInput, (Display *, Window, long)) \
    X(int, XStoreName, (Display *, Window, const char *)) \
    X(Atom, XInternAtom, (Display *, const char *, int)) \
    X(int, XSetWMProtocols, (Display *, Window, Atom *, int)) \
    X(int, XPending, (Display *)) \
    X(int, XNextEvent, (Display *, XEvent *)) \
    X(GC, XCreateGC, (Display *, XID, unsigned long, void *)) \
    X(int, XSetForeground, (Display *, GC, unsigned long)) \
    X(int, XSetFont, (Display *, GC, Font)) \
    X(Font, XLoadFont, (Display *, const char *)) \
    X(int, XFillRectangle, (Display *, XID, GC, int, int, unsigned, unsigned)) \
    X(int, XDrawRectangle, (Display *, XID, GC, int, int, unsigned, unsigned)) \
    X(int, XDrawString, (Display *, XID, GC, int, int, const char *, int)) \
    X(Pixmap, XCreatePixmap, (Display *, XID, unsigned, unsigned, unsigned)) \
    X(int, XFreePixmap, (Display *, Pixmap)) \
    X(int, XCopyArea, (Display *, XID, XID, GC, int, int, unsigned, unsigned, int, int)) \
    X(void *, XCreateImage, (Display *, void *, unsigned, int, int, char *, unsigned, unsigned, int, int)) \
    X(int, XPutImage, (Display *, XID, GC, void *, int, int, int, int, unsigned, unsigned)) \
    X(int, XFree, (void *)) \
    X(int, XFlush, (Display *)) \
    X(int, XConnectionNumber, (Display *)) \
    X(unsigned long, XLookupKeysym, (XKeyEvent *, int)) \
    X(int, XSendEvent, (Display *, Window, int, long, XEvent *)) \
    X(int, XChangeProperty, (Display *, Window, Atom, Atom, int, int, const unsigned char *, int)) \
    X(int, XResizeWindow, (Display *, Window, unsigned, unsigned)) \
    X(int, XUnmapWindow, (Display *, Window)) \
    X(int, XSync, (Display *, int))

#define DECL(ret, name, args) static ret(*p_##name) args;
X_FUNCS(DECL)
#undef DECL

static int load_x11(void)
{
    void *h = dlopen("libX11.so.6", RTLD_NOW);
    if (!h)
        h = dlopen("libX11.so", RTLD_NOW);
    if (!h)
        return -1;
#define LOADF(ret, name, args) \
    if (!(*(void **)&p_##name = dlsym(h, #name))) \
        return -1;
    X_FUNCS(LOADF)
#undef LOADF
    return 0;
}

/* ---- state ---- */

#define BAR_H 64
enum { B_RECORD, B_SNAP, B_INPUT, B_DEINT, B_MONITOR, B_QUALITY, B_FOLDER, B_OPEN, B_COUNT };
enum { XK_Escape = 0xff1b, XK_F11 = 0xffc8, XK_Return = 0xff0d };

static struct {
    app_t *app;
    Display *dpy;
    int scr;
    Window wnd, root;
    GC gc;
    Pixmap bar;
    int bar_w;
    int w, h, fullscreen, depth;
    void *visual;
    uint32_t *frame; /* latest picture, app_width x app_height */
    int fw, fh;
    uint64_t seq;
    uint32_t *scaled; /* window sized backing for the preview */
    int sw, sh;
    void *img;
    int dirty_picture, dirty_bar;
    int svideo, deint, monitor, quality;
    char folder[1024];
    char status[600];
    double level[2];
    struct { int x, y, w, h; } btn[B_COUNT];
    Atom wm_delete, wm_state, wm_fullscreen;
    pthread_mutex_t mu;
    int finishing;
    int done_flag, done_ok;
    char done_path[1100];
    unsigned long last_click;
} G = { .deint = 1, .monitor = 1, .quality = 1, .mu = PTHREAD_MUTEX_INITIALIZER };

static const char *const QUALITY_NAME[] = { "6 Mbit/s", "8 Mbit/s", "9.5 Mbit/s" };

static void set_status(const char *s)
{
    snprintf(G.status, sizeof(G.status), "%s", s ? s : "");
    G.dirty_bar = 1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static unsigned long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)(t.tv_sec * 1000UL + t.tv_nsec / 1000000UL);
}

/* ---- helper programs ---- */

static void spawn(const char *a, const char *b, const char *c)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", 2);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        execlp(a, a, b, c, (char *)NULL);
        _exit(127);
    }
}

static void open_folder(void)
{
    mkdir(G.folder, 0755);
    spawn("xdg-open", G.folder, NULL);
}

static void choose_folder(void)
{
    char cmd[1400], out[1100] = "";
    const char *tools[] = { "zenity --file-selection --directory --title='Recordings folder' 2>/dev/null",
                            "kdialog --getexistingdirectory '%s' 2>/dev/null" };
    for (int i = 0; i < 2; i++) {
        if (i == 1)
            snprintf(cmd, sizeof(cmd), tools[1], G.folder);
        else
            snprintf(cmd, sizeof(cmd), "%s", tools[0]);
        FILE *p = popen(cmd, "r");
        if (!p)
            continue;
        if (fgets(out, sizeof(out), p)) {
            out[strcspn(out, "\r\n")] = 0;
            if (out[0]) {
                snprintf(G.folder, sizeof(G.folder), "%s", out);
                app_set_setting_str("folder", G.folder);
                pclose(p);
                G.dirty_bar = 1;
                return;
            }
        }
        if (pclose(p) == 0)
            return; /* the user cancelled */
    }
    set_status("Install zenity or kdialog to pick a folder, or edit the folder in the config file");
}

/* ---- actions ---- */

static void rec_done(void *user, int ok, const char *path)
{
    (void)user;
    pthread_mutex_lock(&G.mu);
    snprintf(G.done_path, sizeof(G.done_path), "%s", path ? path : "");
    G.done_ok = ok;
    G.done_flag = 1;
    pthread_mutex_unlock(&G.mu);
}

static void toggle_record(void)
{
    if (G.finishing)
        return;
    if (app_is_recording(G.app)) {
        G.finishing = 1;
        set_status("Finishing the file...");
        app_record_stop(G.app, rec_done, NULL);
        return;
    }
    char path[1100], err[300], msg[1300];
    if (app_record_start(G.app, G.folder, G.quality, G.deint, path, sizeof(path), err, sizeof(err))) {
        set_status(err);
        return;
    }
    snprintf(msg, sizeof(msg), "Recording %s MPEG-2 to %s", G.deint ? "deinterlaced" : "interlaced", base_name(path));
    set_status(msg);
}

static void snapshot(void)
{
    char path[1100], err[300], msg[1300];
    if (app_snapshot(G.app, G.folder, path, sizeof(path), err, sizeof(err)))
        set_status(err);
    else {
        snprintf(msg, sizeof(msg), "Saved %s", base_name(path));
        set_status(msg);
    }
}

static void toggle_fullscreen(void)
{
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = G.wnd;
    e.xclient.message_type = G.wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = G.fullscreen ? 0 : 1;
    e.xclient.data.l[1] = (long)G.wm_fullscreen;
    p_XSendEvent(G.dpy, G.root, 0, SubstructureNotifyMask | SubstructureRedirectMask, &e);
    p_XFlush(G.dpy);
    G.fullscreen = !G.fullscreen;
    G.dirty_picture = G.dirty_bar = 1;
}

static void set_input(int svideo)
{
    G.svideo = svideo;
    app_set_input(G.app, svideo ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE);
    app_set_setting_int("input", svideo);
    G.dirty_bar = 1;
}

static void press(int b)
{
    int rec = app_is_recording(G.app);
    switch (b) {
    case B_RECORD: toggle_record(); break;
    case B_SNAP: snapshot(); break;
    case B_INPUT: set_input(!G.svideo); break;
    case B_DEINT:
        if (rec)
            break;
        G.deint = !G.deint;
        app_set_deinterlace(G.app, G.deint);
        app_set_setting_int("deinterlace", G.deint);
        break;
    case B_MONITOR:
        G.monitor = !G.monitor;
        app_set_monitor(G.app, G.monitor);
        app_set_setting_int("monitor", G.monitor);
        break;
    case B_QUALITY:
        if (rec)
            break;
        G.quality = (G.quality + 1) % 3;
        app_set_setting_int("quality", G.quality);
        break;
    case B_FOLDER:
        if (!rec)
            choose_folder();
        break;
    case B_OPEN: open_folder(); break;
    }
    G.dirty_bar = 1;
}

/* ---- drawing ---- */

static unsigned long rgb(int r, int g, int b) { return ((unsigned long)r << 16) | (g << 8) | b; }

static void text(XID d, int x, int y, const char *s, unsigned long col)
{
    p_XSetForeground(G.dpy, G.gc, col);
    p_XDrawString(G.dpy, d, G.gc, x, y, s, (int)strlen(s));
}

static int area_h(void) { return G.fullscreen ? G.h : G.h - BAR_H; }

static void picture_rect(int *x, int *y, int *w, int *h)
{
    int aw = G.w, ah = area_h();
    *w = aw;
    *h = aw * 3 / 4;
    if (*h > ah) {
        *h = ah;
        *w = ah * 4 / 3;
    }
    *x = (aw - *w) / 2;
    *y = (ah - *h) / 2;
}

static void draw_picture(void)
{
    if (G.w < 2 || area_h() < 2)
        return;
    if (!G.scaled || G.sw != G.w || G.sh != area_h()) {
        if (G.img) {
            ((XImageHead *)G.img)->data = NULL;
            p_XFree(G.img);
            G.img = NULL;
        }
        free(G.scaled);
        G.sw = G.w;
        G.sh = area_h();
        G.scaled = calloc((size_t)G.sw * G.sh, 4);
        if (!G.scaled)
            return;
        G.img = p_XCreateImage(G.dpy, G.visual, (unsigned)G.depth, 2, 0, (char *)G.scaled, (unsigned)G.sw,
                               (unsigned)G.sh, 32, 0);
    }
    if (!G.img)
        return;
    memset(G.scaled, 0, (size_t)G.sw * G.sh * 4);
    int px, py, pw, ph;
    picture_rect(&px, &py, &pw, &ph);
    if (G.frame && pw > 0 && ph > 0) {
        int *xm = malloc(sizeof(int) * (size_t)pw);
        if (xm) {
            for (int x = 0; x < pw; x++)
                xm[x] = x * G.fw / pw;
            for (int y = 0; y < ph; y++) {
                const uint32_t *src = G.frame + (size_t)(y * G.fh / ph) * G.fw;
                uint32_t *dst = G.scaled + (size_t)(py + y) * G.sw + px;
                for (int x = 0; x < pw; x++)
                    dst[x] = src[xm[x]];
            }
            free(xm);
        }
    }
    p_XPutImage(G.dpy, G.wnd, G.gc, G.img, 0, 0, 0, 0, (unsigned)G.sw, (unsigned)G.sh);
    G.dirty_picture = 0;
}

static void button(Pixmap d, int i, int x, int y, int w, const char *label, int on, int enabled)
{
    G.btn[i].x = x, G.btn[i].y = y, G.btn[i].w = w, G.btn[i].h = 24;
    p_XSetForeground(G.dpy, G.gc, on ? rgb(70, 110, 170) : rgb(70, 70, 76));
    p_XFillRectangle(G.dpy, d, G.gc, x, y, (unsigned)w, 24);
    p_XSetForeground(G.dpy, G.gc, rgb(110, 110, 118));
    p_XDrawRectangle(G.dpy, d, G.gc, x, y, (unsigned)w - 1, 23);
    int tw = (int)strlen(label) * 6;
    text(d, x + (w - tw) / 2, y + 16, label, enabled ? rgb(235, 235, 235) : rgb(130, 130, 130));
}

static void meter(Pixmap d, int x, int y, int w, double lv)
{
    p_XSetForeground(G.dpy, G.gc, rgb(20, 20, 22));
    p_XFillRectangle(G.dpy, d, G.gc, x, y, (unsigned)w, 8);
    int fill = (int)(lv / 60.0 * w);
    if (fill > w)
        fill = w;
    for (int i = 0; i < fill; i += 3) {
        double f = (double)i / w;
        p_XSetForeground(G.dpy, G.gc, f > 0.92 ? rgb(220, 60, 50) : f > 0.75 ? rgb(230, 190, 60) : rgb(70, 200, 90));
        p_XFillRectangle(G.dpy, d, G.gc, x + i, y, 2, 8);
    }
}

static void draw_bar(void)
{
    if (G.fullscreen || G.w < 2)
        return;
    if (!G.bar || G.bar_w != G.w) {
        if (G.bar)
            p_XFreePixmap(G.dpy, G.bar);
        G.bar = p_XCreatePixmap(G.dpy, G.wnd, (unsigned)G.w, BAR_H, (unsigned)G.depth);
        G.bar_w = G.w;
    }
    Pixmap d = G.bar;
    int rec = app_is_recording(G.app), x = 8;
    char buf[1200];
    p_XSetForeground(G.dpy, G.gc, rgb(40, 40, 44));
    p_XFillRectangle(G.dpy, d, G.gc, 0, 0, (unsigned)G.w, BAR_H);

    button(d, B_RECORD, x, 6, 96, G.finishing ? "Finishing..." : rec ? "Stop (R)" : "Record (R)", rec, !G.finishing);
    x += 102;
    button(d, B_SNAP, x, 6, 96, "Snapshot (S)", 0, 1);
    x += 102;
    button(d, B_INPUT, x, 6, 120, G.svideo ? "Input: S-Video" : "Input: Composite", 0, 1);
    x += 126;
    button(d, B_DEINT, x, 6, 100, G.deint ? "Deinterlace" : "Interlaced", G.deint, !rec);
    x += 106;
    button(d, B_MONITOR, x, 6, 76, G.monitor ? "Monitor" : "Muted", G.monitor, 1);
    x += 82;
    button(d, B_QUALITY, x, 6, 84, QUALITY_NAME[G.quality], 0, !rec);
    x += 90;
    button(d, B_FOLDER, x, 6, 70, "Folder...", 0, !rec);
    x += 76;
    button(d, B_OPEN, x, 6, 56, "Open", 0, 1);
    x += 62;

    int s = rec ? (int)app_record_seconds(G.app) : 0;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    text(d, G.w - 62, 22, buf, rec ? rgb(240, 90, 80) : rgb(200, 200, 200));

    text(d, 8, 50, "L", rgb(170, 170, 170));
    meter(d, 20, 42, 120, G.level[0]);
    text(d, 150, 50, "R", rgb(170, 170, 170));
    meter(d, 162, 42, 120, G.level[1]);
    snprintf(buf, sizeof(buf), "%s", G.status);
    int maxc = (G.w - 300) / 6;
    if (maxc > 0 && (int)strlen(buf) > maxc)
        buf[maxc] = 0;
    text(d, 296, 50, buf, rgb(200, 200, 200));

    p_XCopyArea(G.dpy, d, G.wnd, G.gc, 0, 0, (unsigned)G.w, BAR_H, 0, G.h - BAR_H);
    G.dirty_bar = 0;
}

static void tick_ui(void)
{
    double db[2];
    app_peaks(G.app, db);
    for (int c = 0; c < 2; c++) {
        double v = db[c] + 60, decayed = G.level[c] - 1.5;
        G.level[c] = v > decayed ? v : decayed < 0 ? 0 : decayed;
    }
    if (!G.fullscreen)
        G.dirty_bar = 1;
}

static void update_title(void)
{
    char t[200];
    snprintf(t, sizeof(t), "%s - %dx%d %s", APP_NAME, app_width(G.app), app_height(G.app),
             app_is_pal(G.app) ? "PAL" : "NTSC");
    p_XStoreName(G.dpy, G.wnd, t);
}

/* ---- main ---- */

static void handle(XEvent *e)
{
    switch (e->type) {
    case Expose:
        G.dirty_picture = G.dirty_bar = 1;
        break;
    case ConfigureNotify:
        G.w = e->xconfigure.width;
        G.h = e->xconfigure.height;
        G.dirty_picture = G.dirty_bar = 1;
        break;
    case ClientMessage:
        if ((Atom)e->xclient.data.l[0] == G.wm_delete)
            G.finishing = -1; /* quit */
        break;
    case KeyPress: {
        unsigned long k = p_XLookupKeysym(&e->xkey, 0);
        if (k == 'r' || k == 'R')
            press(B_RECORD);
        else if (k == 's' || k == 'S')
            press(B_SNAP);
        else if (k == 'i' || k == 'I')
            press(B_INPUT);
        else if (k == 'm' || k == 'M')
            press(B_MONITOR);
        else if (k == 'f' || k == 'F' || k == XK_F11)
            toggle_fullscreen();
        else if (k == XK_Escape && G.fullscreen)
            toggle_fullscreen();
        else if (k == 'q' || k == 'Q')
            G.finishing = -1;
        break;
    }
    case ButtonPress: {
        int x = e->xbutton.x, y = e->xbutton.y - (G.h - BAR_H);
        if (e->xbutton.keycode != 1)
            break;
        if (!G.fullscreen && y >= 0) {
            for (int i = 0; i < B_COUNT; i++)
                if (x >= G.btn[i].x && x < G.btn[i].x + G.btn[i].w && y >= G.btn[i].y && y < G.btn[i].y + G.btn[i].h)
                    press(i);
        } else {
            unsigned long t = now_ms();
            if (t - G.last_click < 400)
                toggle_fullscreen();
            G.last_click = t;
        }
        break;
    }
    }
}

int main(int argc, char **argv)
{
    char err[400];
    dvc100_input_t input = app_setting_int("input", 0) == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE;
    dvc100_norm_t norm = DVC100_NORM_AUTO;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--svideo")) input = DVC100_INPUT_SVIDEO;
        if (!strcmp(argv[i], "--composite")) input = DVC100_INPUT_COMPOSITE;
        if (!strcmp(argv[i], "--pal")) norm = DVC100_NORM_PAL;
        if (!strcmp(argv[i], "--ntsc")) norm = DVC100_NORM_NTSC;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    if (load_x11() || !(G.dpy = p_XOpenDisplay(NULL))) {
        fprintf(stderr, "%s: cannot open an X display (libX11 and a running X11 or XWayland session are needed)\n",
                APP_NAME);
        return 1;
    }
    G.app = app_open(input, norm, err, sizeof(err));
    if (!G.app) {
        fprintf(stderr, "No Dazzle capture device available: %s\n", err);
        return 1;
    }
    G.svideo = input == DVC100_INPUT_SVIDEO;
    G.deint = app_setting_int("deinterlace", 1);
    G.monitor = app_setting_int("monitor", 1);
    G.quality = app_setting_int("quality", 1);
    if (G.quality < 0 || G.quality > 2)
        G.quality = 1;
    app_setting_str("folder", G.folder, sizeof(G.folder), "");
    if (!G.folder[0])
        app_default_folder(G.folder, sizeof(G.folder));
    app_set_deinterlace(G.app, G.deint);
    app_set_monitor(G.app, G.monitor);
    set_status(app_audio_problem(G.app));

    G.scr = p_XDefaultScreen(G.dpy);
    G.root = p_XRootWindow(G.dpy, G.scr);
    G.depth = p_XDefaultDepth(G.dpy, G.scr);
    G.visual = p_XDefaultVisual(G.dpy, G.scr);
    G.w = 800;
    G.h = 600 + BAR_H;
    G.wnd = p_XCreateSimpleWindow(G.dpy, G.root, 0, 0, (unsigned)G.w, (unsigned)G.h, 0, 0, 0);
    G.gc = p_XCreateGC(G.dpy, G.wnd, 0, NULL);
    p_XSetFont(G.dpy, G.gc, p_XLoadFont(G.dpy, "fixed"));
    G.wm_delete = p_XInternAtom(G.dpy, "WM_DELETE_WINDOW", 0);
    G.wm_state = p_XInternAtom(G.dpy, "_NET_WM_STATE", 0);
    G.wm_fullscreen = p_XInternAtom(G.dpy, "_NET_WM_STATE_FULLSCREEN", 0);
    p_XSetWMProtocols(G.dpy, G.wnd, &G.wm_delete, 1);
    p_XSelectInput(G.dpy, G.wnd, KeyPressMask | ButtonPressMask | ExposureMask | StructureNotifyMask);
    update_title();
    p_XMapWindow(G.dpy, G.wnd);
    p_XFlush(G.dpy);

    int fd = p_XConnectionNumber(G.dpy);
    unsigned long last_ui = 0;
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        poll(&pf, 1, 15);
        while (p_XPending(G.dpy)) {
            XEvent e;
            p_XNextEvent(G.dpy, &e);
            handle(&e);
        }
        if (G.finishing == -1) {
            if (app_is_recording(G.app))
                set_status("Finishing the recording before quitting...");
            break;
        }
        int w = app_width(G.app), h = app_height(G.app);
        if (w != G.fw || h != G.fh) {
            free(G.frame);
            G.frame = calloc((size_t)w * h, 4);
            G.fw = w;
            G.fh = h;
            update_title();
        }
        if (G.frame && app_get_frame(G.app, G.frame, &G.seq))
            G.dirty_picture = 1;
        pthread_mutex_lock(&G.mu);
        if (G.done_flag) {
            G.done_flag = 0;
            G.finishing = 0;
            char msg[1300];
            if (G.done_ok) {
                snprintf(msg, sizeof(msg), "Saved %s", base_name(G.done_path));
                set_status(msg);
            } else
                set_status("Recording failed (ffmpeg error)");
        }
        pthread_mutex_unlock(&G.mu);
        unsigned long t = now_ms();
        if (t - last_ui >= 50) {
            last_ui = t;
            tick_ui();
        }
        if (G.dirty_picture)
            draw_picture();
        if (G.dirty_bar)
            draw_bar();
        p_XFlush(G.dpy);
    }
    app_close(G.app); /* finishes a recording still in progress */
    p_XCloseDisplay(G.dpy);
    return 0;
}
