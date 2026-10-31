/*
 * Linux audio through ALSA. libasound is loaded at run time, so building
 * needs no ALSA headers and the programs still run (without system audio)
 * where it is missing.
 *
 * The capture box shows up as an snd-usb-audio card; /proc/asound/cardN/usbid
 * holds its "vvvv:pppp". Playback goes to the "default" device, which is the
 * sound server (PipeWire/PulseAudio) on desktop systems.
 */
#include "sysaudio.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct snd_pcm snd_pcm_t;
enum { PCM_PLAYBACK = 0, PCM_CAPTURE = 1, FMT_S16_LE = 2, ACCESS_RW_INTERLEAVED = 3 };

static struct {
    int loaded;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
    long (*readi)(snd_pcm_t *, void *, unsigned long);
    long (*writei)(snd_pcm_t *, const void *, unsigned long);
    int (*recover)(snd_pcm_t *, int, int);
    int (*close)(snd_pcm_t *);
} A;

static void alsa_dlopen(void)
{
    void *h = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return;
    *(void **)&A.open = dlsym(h, "snd_pcm_open");
    *(void **)&A.set_params = dlsym(h, "snd_pcm_set_params");
    *(void **)&A.readi = dlsym(h, "snd_pcm_readi");
    *(void **)&A.writei = dlsym(h, "snd_pcm_writei");
    *(void **)&A.recover = dlsym(h, "snd_pcm_recover");
    *(void **)&A.close = dlsym(h, "snd_pcm_close");
    A.loaded = A.open && A.set_params && A.readi && A.writei && A.recover && A.close;
}

static int alsa_load(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, alsa_dlopen);
    return A.loaded;
}

const char *sa_backend_name(void) { return "ALSA"; }

static int read_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(buf, (int)len, f) != NULL;
    fclose(f);
    return ok ? 0 : -1;
}

/* Card number of the box: by USB id, else by name in /proc/asound/cards. */
static int find_card(uint16_t vid, uint16_t pid, const char *product)
{
    char path[64], buf[256], want[16];
    const char *env = getenv("DVC100_AUDIO_DEVICE");
    snprintf(want, sizeof(want), "%04x:%04x", vid, pid);
    if (!(env && *env)) {
        for (int n = 0; n < 32; n++) {
            snprintf(path, sizeof(path), "/proc/asound/card%d/usbid", n);
            if (read_line(path, buf, sizeof(buf)) == 0 && !strncmp(buf, want, 9))
                return n;
        }
    }
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f)
        return -1;
    int card = -1, cur = -1;
    while (card < 0 && fgets(buf, sizeof(buf), f)) {
        int n;
        if (sscanf(buf, " %d [", &n) == 1)
            cur = n; /* " N [id   ]: driver - name", then an indented long name line */
        if (cur >= 0 && sa_name_matches(buf, product))
            card = cur;
    }
    fclose(f);
    return card;
}

struct sa_input {
    snd_pcm_t *pcm;
    sa_input_cb cb;
    void *user;
    volatile int quit;
    pthread_t thread;
};

static void *capture_thread(void *arg)
{
    sa_input_t *in = arg;
    int16_t buf[480 * 2]; /* 10 ms */
    while (!in->quit) {
        long n = A.readi(in->pcm, buf, 480);
        if (n < 0) {
            if (A.recover(in->pcm, (int)n, 1) < 0)
                break;
            continue;
        }
        if (n > 0)
            in->cb(buf, (int)n, in->user);
    }
    return NULL;
}

sa_input_t *sa_input_start(uint16_t vid, uint16_t pid, const char *product, sa_input_cb cb, void *user,
                           int *rate)
{
    if (!alsa_load())
        return NULL;
    int card = find_card(vid, pid, product);
    if (card < 0)
        return NULL;
    char name[32];
    snprintf(name, sizeof(name), "plughw:%d,0", card);
    sa_input_t *in = calloc(1, sizeof(*in));
    if (!in)
        return NULL;
    if (A.open(&in->pcm, name, PCM_CAPTURE, 0) < 0) {
        free(in);
        return NULL;
    }
    if (A.set_params(in->pcm, FMT_S16_LE, ACCESS_RW_INTERLEAVED, 2, 48000, 1, 100000) < 0) {
        A.close(in->pcm);
        free(in);
        return NULL;
    }
    in->cb = cb;
    in->user = user;
    if (pthread_create(&in->thread, NULL, capture_thread, in)) {
        A.close(in->pcm);
        free(in);
        return NULL;
    }
    if (rate)
        *rate = 48000;
    return in;
}

void sa_input_stop(sa_input_t *in)
{
    if (!in)
        return;
    in->quit = 1;
    pthread_join(in->thread, NULL);
    A.close(in->pcm);
    free(in);
}

struct sa_output {
    snd_pcm_t *pcm;
    sa_output_cb cb;
    void *user;
    volatile int quit;
    pthread_t thread;
};

static void *playback_thread(void *arg)
{
    sa_output_t *out = arg;
    int16_t buf[480 * 2];
    while (!out->quit) {
        out->cb(buf, 480, out->user);
        long n = A.writei(out->pcm, buf, 480);
        if (n < 0 && A.recover(out->pcm, (int)n, 1) < 0)
            break;
    }
    return NULL;
}

sa_output_t *sa_output_start(int rate, sa_output_cb cb, void *user)
{
    if (!alsa_load())
        return NULL;
    sa_output_t *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    if (A.open(&out->pcm, "default", PCM_PLAYBACK, 0) < 0) {
        free(out);
        return NULL;
    }
    if (A.set_params(out->pcm, FMT_S16_LE, ACCESS_RW_INTERLEAVED, 2, (unsigned)rate, 1, 60000) < 0 ||
        (out->cb = cb, out->user = user, pthread_create(&out->thread, NULL, playback_thread, out))) {
        A.close(out->pcm);
        free(out);
        return NULL;
    }
    return out;
}

void sa_output_stop(sa_output_t *out)
{
    if (!out)
        return;
    out->quit = 1;
    pthread_join(out->thread, NULL);
    A.close(out->pcm);
    free(out);
}
