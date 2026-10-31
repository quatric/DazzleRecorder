/* Dazzle Capture engine for the Windows and Linux GUIs; see app_core.h. */
#include "app_core.h"
#include "recorder.h"
#include "sysaudio.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#define DIR_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define DIR_SEP '/'
#endif

#define RING_FRAMES 32768 /* power of two, ~680 ms at 48 kHz */

struct app {
    dvc100_t *dev;
    int w, h, pal;
    volatile int deinterlace;
    volatile int widescreen;

    pthread_mutex_t frame_lock;
    uint32_t *rgb, *spare; /* latest preview frame and the one being converted */
    uint8_t *blend;        /* one deinterlaced YUYV line */
    uint64_t seq;

    int16_t ring[RING_FRAMES * 2];
    _Alignas(8) volatile uint64_t ring_w;
    _Alignas(8) volatile uint64_t ring_r;
    volatile int peak[2];
    int audio_rate, audio_ok;
    char audio_problem[160];
    sa_output_t *out;
    volatile int monitor;

    pthread_mutex_t rec_lock;
    recorder_t *rec;
    struct timespec rec_t0;
    uint64_t silence_frames, silence_fed; /* video frames and samples of silence while there is no audio */
    char rec_path[1024];
};

/* ---- video ---- */

static inline uint32_t yuv_rgb(int y, int u, int v)
{
    int c = 298 * (y - 16), d = u - 128, e = v - 128;
    int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static void frame_cb(const dvc100_frame_t *f, void *user)
{
    app_t *a = user;
    pthread_mutex_lock(&a->rec_lock);
    if (a->rec) {
        rec_video(a->rec, f->data); /* full interlaced frame; ffmpeg deinterlaces properly */
        if (!a->audio_ok) {
            /* no audio input: feed 48 kHz silence so the recording still has its audio clock
             * (1920 samples per PAL frame, 1601.6 per NTSC frame) */
            static const int16_t zeros[2 * 1922] = { 0 };
            uint64_t due = ++a->silence_frames * (a->pal ? 1920 : 8008) / (a->pal ? 1 : 5);
            int n = (int)(due - a->silence_fed);
            rec_audio(a->rec, zeros, n);
            a->silence_fed = due;
        }
    }
    pthread_mutex_unlock(&a->rec_lock);

    const size_t stride = (size_t)f->width * 2;
    for (int y = 0; y < f->height; y++) {
        const uint8_t *s = f->data + (size_t)y * stride;
        if (a->deinterlace && y + 1 < f->height) {
            /* preview only: line blend, cheap and removes combing on motion */
            const uint8_t *n = s + stride;
            for (size_t i = 0; i < stride; i++)
                a->blend[i] = (uint8_t)((s[i] + n[i] + 1) >> 1);
            s = a->blend;
        }
        uint32_t *d = a->spare + (size_t)y * f->width;
        for (int x = 0; x < f->width; x += 2, s += 4) {
            d[x] = yuv_rgb(s[0], s[1], s[3]);
            d[x + 1] = yuv_rgb(s[2], s[1], s[3]);
        }
    }
    pthread_mutex_lock(&a->frame_lock);
    uint32_t *t = a->rgb;
    a->rgb = a->spare;
    a->spare = t;
    a->seq++;
    pthread_mutex_unlock(&a->frame_lock);
}

int app_get_frame(app_t *a, uint32_t *buf, uint64_t *seq)
{
    int copied = 0;
    pthread_mutex_lock(&a->frame_lock);
    if (a->seq != *seq) {
        memcpy(buf, a->rgb, (size_t)a->w * a->h * 4);
        *seq = a->seq;
        copied = 1;
    }
    pthread_mutex_unlock(&a->frame_lock);
    return copied;
}

/* ---- audio ---- */

static void audio_cb(const int16_t *s, int frames, void *user)
{
    app_t *a = user;
    uint64_t w = __atomic_load_n(&a->ring_w, __ATOMIC_ACQUIRE);
    int pl = 0, pr = 0;
    for (int i = 0; i < frames; i++, w++) {
        int16_t l = s[2 * i], r = s[2 * i + 1];
        a->ring[(w % RING_FRAMES) * 2] = l;
        a->ring[(w % RING_FRAMES) * 2 + 1] = r;
        if (abs(l) > pl) pl = abs(l);
        if (abs(r) > pr) pr = abs(r);
    }
    __atomic_store_n(&a->ring_w, w, __ATOMIC_RELEASE);
    if (pl > a->peak[0]) a->peak[0] = pl;
    if (pr > a->peak[1]) a->peak[1] = pr;

    pthread_mutex_lock(&a->rec_lock);
    if (a->rec)
        rec_audio(a->rec, s, frames);
    pthread_mutex_unlock(&a->rec_lock);
}

/* monitor playback: about 60 ms behind the input, resynced if it drifts */
static void output_cb(int16_t *out, int frames, void *user)
{
    app_t *a = user;
    const uint64_t target = (uint64_t)a->audio_rate * 60 / 1000, maxlag = (uint64_t)a->audio_rate / 4;
    uint64_t w = __atomic_load_n(&a->ring_w, __ATOMIC_ACQUIRE), rd = a->ring_r;
    if (w - rd > maxlag || rd > w)
        rd = w > target ? w - target : 0;
    for (int i = 0; i < frames; i++) {
        if (rd < w && a->monitor) {
            out[2 * i] = a->ring[(rd % RING_FRAMES) * 2];
            out[2 * i + 1] = a->ring[(rd % RING_FRAMES) * 2 + 1];
        } else {
            out[2 * i] = out[2 * i + 1] = 0;
        }
        if (rd < w)
            rd++;
    }
    a->ring_r = rd;
}

void app_peaks(app_t *a, double db[2])
{
    for (int c = 0; c < 2; c++) {
        int p = __atomic_exchange_n(&a->peak[c], 0, __ATOMIC_RELAXED);
        db[c] = p > 0 ? 20 * log10(p / 32768.0) : -60;
        if (db[c] < -60)
            db[c] = -60;
    }
}

void app_set_monitor(app_t *a, int on) { a->monitor = on; }
const char *app_audio_problem(app_t *a) { return a->audio_problem[0] ? a->audio_problem : NULL; }

/* ---- device ---- */

app_t *app_open(dvc100_input_t input, dvc100_norm_t norm, char *err, int errlen)
{
    dvc100_t *dev = NULL;
    int r = dvc100_open(&dev, 0, 0);
    if (!r)
        r = dvc100_init(dev, input, norm);
    if (r) {
#ifdef _WIN32
        const char *hint = "program, and that its video interface (Interface 0) uses the WinUSB driver (see README).";
#elif defined(__linux__)
        const char *hint = "program, and that you may access it (install the udev rule, see README).";
#else
        const char *hint = "app.";
#endif
        snprintf(err, (size_t)errlen, "%s.\n\nCheck that the device is plugged in and not in use by another %s",
                 dvc100_strerror(r), hint);
        dvc100_close(dev);
        return NULL;
    }
    app_t *a = calloc(1, sizeof(*a));
    a->dev = dev;
    a->pal = dvc100_current_norm(dev) == DVC100_NORM_PAL;
    a->w = 720;
    a->h = a->pal ? 576 : 480;
    a->rgb = calloc((size_t)a->w * a->h, 4);
    a->spare = calloc((size_t)a->w * a->h, 4);
    a->blend = malloc((size_t)a->w * 2);
    a->audio_rate = 48000;
    pthread_mutex_init(&a->frame_lock, NULL);
    pthread_mutex_init(&a->rec_lock, NULL);

    if ((r = dvc100_start(dev, frame_cb, a))) {
        snprintf(err, (size_t)errlen, "Video streaming failed: %s", dvc100_strerror(r));
        app_close(a);
        return NULL;
    }
    if ((r = dvc100_audio_start(dev, audio_cb, a, &a->audio_rate)) == 0) {
        a->audio_ok = 1;
        a->ring_r = a->ring_w;
        a->out = sa_output_start(a->audio_rate, output_cb, a);
        if (!a->out)
            snprintf(a->audio_problem, sizeof(a->audio_problem), "No audio output device for monitoring");
    } else {
        snprintf(a->audio_problem, sizeof(a->audio_problem), "Audio unavailable: %s", dvc100_strerror(r));
    }
    return a;
}

void app_close(app_t *a)
{
    if (!a)
        return;
    pthread_mutex_lock(&a->rec_lock);
    recorder_t *rec = a->rec;
    a->rec = NULL;
    pthread_mutex_unlock(&a->rec_lock);
    if (rec)
        rec_close(rec);
    sa_output_stop(a->out);
    dvc100_close(a->dev); /* stops video and audio */
    pthread_mutex_destroy(&a->frame_lock);
    pthread_mutex_destroy(&a->rec_lock);
    free(a->rgb);
    free(a->spare);
    free(a->blend);
    free(a);
}

int app_width(app_t *a) { return a->w; }
int app_height(app_t *a) { return a->h; }
int app_is_pal(app_t *a) { return a->pal; }
void app_set_input(app_t *a, dvc100_input_t input) { dvc100_set_input(a->dev, input); }
void app_set_deinterlace(app_t *a, int on) { a->deinterlace = on; }
void app_set_widescreen(app_t *a, int on) { a->widescreen = on; }

/* ---- files ---- */

FILE *app_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

static int make_dir(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024))
        return -1;
    return CreateDirectoryW(wp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || access(path, F_OK) == 0 ? 0 : -1;
#endif
}

void app_timestamped_path(const char *folder, const char *ext, char *out, int len)
{
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d at %H.%M.%S", &tm);
    size_t fl = strlen(folder);
    int sep = fl && folder[fl - 1] != '/' && folder[fl - 1] != '\\';
    snprintf(out, (size_t)len, "%s%s" APP_NAME " %s.%s", folder, sep ? (DIR_SEP == '/' ? "/" : "\\") : "", stamp,
             ext);
}

void app_default_folder(char *out, int len)
{
#ifdef _WIN32
    static const GUID videos = { 0x18989b1d, 0x99b5, 0x455b, { 0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc } };
    PWSTR w = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&videos, 0, NULL, &w)))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
    CoTaskMemFree(w);
    if (!out[0]) {
        const char *p = getenv("USERPROFILE");
        snprintf(out, (size_t)len, "%s", p ? p : ".");
    }
#else
    const char *home = getenv("HOME");
    char cfg[1024], line[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    snprintf(cfg, sizeof(cfg), "%s/user-dirs.dirs", xdg && *xdg ? xdg : "");
    if (!(xdg && *xdg))
        snprintf(cfg, sizeof(cfg), "%s/.config/user-dirs.dirs", home ? home : "");
    snprintf(out, (size_t)len, "%s/Videos", home ? home : ".");
    FILE *f = fopen(cfg, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *v = strstr(line, "XDG_VIDEOS_DIR=\"");
            if (v != line)
                continue;
            v += 16;
            char *end = strchr(v, '"');
            if (end)
                *end = 0;
            if (!strncmp(v, "$HOME", 5))
                snprintf(out, (size_t)len, "%s%s", home ? home : "", v + 5);
            else
                snprintf(out, (size_t)len, "%s", v);
        }
        fclose(f);
    }
    if (access(out, F_OK) != 0)
        snprintf(out, (size_t)len, "%s", home ? home : ".");
#endif
}

/* ---- settings ---- */

static void settings_path(char *out, size_t len)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s\\" APP_NAME, base ? base : ".");
    make_dir(dir);
    snprintf(out, len, "%s\\settings.ini", dir);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char base[1024], dir[1100];
    if (xdg && *xdg)
        snprintf(base, sizeof(base), "%s", xdg);
    else
        snprintf(base, sizeof(base), "%s/.config", home ? home : ".");
    make_dir(base);
    snprintf(dir, sizeof(dir), "%s/dazzle-capture", base);
    make_dir(dir);
    snprintf(out, len, "%s/settings.ini", dir);
#endif
}

void app_setting_str(const char *key, char *out, int len, const char *def)
{
    char path[1200], line[1200];
    size_t kl = strlen(key);
    snprintf(out, (size_t)len, "%s", def ? def : "");
    settings_path(path, sizeof(path));
    FILE *f = app_fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) || line[kl] != '=')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, (size_t)len, "%s", line + kl + 1);
    }
    fclose(f);
}

int app_setting_int(const char *key, int def)
{
    char v[32];
    app_setting_str(key, v, sizeof(v), "");
    return v[0] ? atoi(v) : def;
}

void app_set_setting_str(const char *key, const char *v)
{
    char path[1200], line[1200];
    size_t kl = strlen(key), used = 0, cap = 4096;
    char *all = malloc(cap);
    if (!all)
        return;
    all[0] = 0;
    settings_path(path, sizeof(path));
    FILE *f = app_fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, key, kl) && line[kl] == '=')
                continue;
            size_t n = strlen(line);
            if (used + n + 1 > cap) {
                char *b = realloc(all, cap *= 2);
                if (!b)
                    break;
                all = b;
            }
            memcpy(all + used, line, n + 1);
            used += n;
        }
        fclose(f);
    }
    if ((f = app_fopen(path, "w"))) {
        fputs(all, f);
        fprintf(f, "%s=%s\n", key, v);
        fclose(f);
    }
    free(all);
}

void app_set_setting_int(const char *key, int v)
{
    char s[32];
    snprintf(s, sizeof(s), "%d", v);
    app_set_setting_str(key, s);
}

/* ---- recording ---- */

int app_record_start(app_t *a, const char *folder, int quality, int deinterlace, char *path, int pathlen,
                     char *err, int errlen)
{
    static const int kbps[] = { 6000, 8000, 9500 };
    make_dir(folder);
    app_timestamped_path(folder, "mpg", path, pathlen);
    recorder_opts_t o = {
        .width = a->w,
        .height = a->h,
        .pal = a->pal,
        .deinterlace = deinterlace,
        .widescreen = a->widescreen,
        .video_kbps = kbps[quality < 0 ? 0 : quality > 2 ? 2 : quality],
        .audio_kbps = 224,
        .audio_rate = a->audio_rate,
    };
    recorder_t *rec = rec_open(path, &o, err, errlen);
    if (!rec)
        return -1;
    pthread_mutex_lock(&a->rec_lock);
    a->rec = rec;
    a->silence_frames = a->silence_fed = 0;
    clock_gettime(CLOCK_MONOTONIC, &a->rec_t0);
    snprintf(a->rec_path, sizeof(a->rec_path), "%s", path);
    pthread_mutex_unlock(&a->rec_lock);
    return 0;
}

typedef struct {
    recorder_t *rec;
    app_done_cb done;
    void *user;
    char path[1024];
} stop_job_t;

static void *stop_thread(void *arg)
{
    stop_job_t *j = arg;
    int ok = rec_close(j->rec) == 0;
    if (j->done)
        j->done(j->user, ok, j->path);
    free(j);
    return NULL;
}

void app_record_stop(app_t *a, app_done_cb done, void *user)
{
    pthread_mutex_lock(&a->rec_lock);
    recorder_t *rec = a->rec;
    a->rec = NULL;
    pthread_mutex_unlock(&a->rec_lock);
    if (!rec)
        return;
    stop_job_t *j = calloc(1, sizeof(*j));
    j->rec = rec;
    j->done = done;
    j->user = user;
    snprintf(j->path, sizeof(j->path), "%s", a->rec_path);
    pthread_t t;
    if (pthread_create(&t, NULL, stop_thread, j) == 0)
        pthread_detach(t);
    else
        stop_thread(j);
}

int app_is_recording(app_t *a)
{
    pthread_mutex_lock(&a->rec_lock);
    int r = a->rec != NULL;
    pthread_mutex_unlock(&a->rec_lock);
    return r;
}

double app_record_seconds(app_t *a)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&a->rec_lock);
    double s = a->rec ? (now.tv_sec - a->rec_t0.tv_sec) + (now.tv_nsec - a->rec_t0.tv_nsec) / 1e9 : 0;
    pthread_mutex_unlock(&a->rec_lock);
    return s;
}

/* ---- snapshot: minimal PNG writer (stored deflate blocks) ---- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(data, 1, len, f);
    uint32_t c = crc32_update(crc32_update(0, (const uint8_t *)type, 4), data, len);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

static int write_png(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = app_fopen(path, "wb");
    if (!f)
        return -1;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0; /* 8-bit RGB */
    png_chunk(f, "IHDR", ihdr, 13);

    size_t raw_len = (size_t)h * (1 + (size_t)w * 3), blocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(zlen), *raw = malloc(raw_len);
    if (!z || !raw) {
        free(z), free(raw), fclose(f);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (1 + (size_t)w * 3)] = 0; /* filter: none */
        memcpy(raw + (size_t)y * (1 + (size_t)w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    uint8_t *p = z;
    *p++ = 0x78, *p++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_len;) {
        uint32_t n = (uint32_t)(raw_len - off > 65535 ? 65535 : raw_len - off);
        *p++ = off + n == raw_len;
        *p++ = (uint8_t)n, *p++ = (uint8_t)(n >> 8), *p++ = (uint8_t)~n, *p++ = (uint8_t)(~n >> 8);
        memcpy(p, raw + off, n);
        for (uint32_t i = 0; i < n; i++) {
            s1 = (s1 + raw[off + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        p += n;
        off += n;
    }
    be32(p, s2 << 16 | s1);
    p += 4;
    png_chunk(f, "IDAT", z, (uint32_t)(p - z));
    png_chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    return fclose(f) ? -1 : 0;
}

int app_snapshot(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen)
{
    /* 720 BT.601 samples span a 4:3 picture: resample to square pixels (or stretch to 16:9) */
    const int oh = a->h, ow = a->widescreen ? oh * 16 / 9 / 2 * 2 : a->pal ? 768 : 640;
    uint32_t *src = malloc((size_t)a->w * a->h * 4);
    uint8_t *dst = malloc((size_t)ow * oh * 3);
    uint64_t seq = (uint64_t)-1;
    if (!src || !dst) {
        free(src), free(dst);
        snprintf(err, (size_t)errlen, "out of memory");
        return -1;
    }
    app_get_frame(a, src, &seq);
    for (int y = 0; y < oh; y++) {
        const uint32_t *row = src + (size_t)y * a->w;
        for (int x = 0; x < ow; x++) {
            double fx = (x + 0.5) * a->w / ow - 0.5;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < a->w ? x0 + 1 : x0;
            double t = fx - x0;
            if (t < 0)
                t = 0;
            for (int c = 0; c < 3; c++) {
                int sh = 16 - 8 * c;
                double v = ((row[x0] >> sh) & 0xff) * (1 - t) + ((row[x1] >> sh) & 0xff) * t;
                dst[((size_t)y * ow + x) * 3 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
    make_dir(folder);
    app_timestamped_path(folder, "png", path, pathlen);
    int r = write_png(path, dst, ow, oh);
    if (r)
        snprintf(err, (size_t)errlen, "Cannot write %s", path);
    free(src);
    free(dst);
    return r;
}
