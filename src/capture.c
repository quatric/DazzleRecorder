/*
 * dvc100-capture: grab video from a Dazzle DVC100 and write it as
 * YUV4MPEG2 (default) or raw YUYV to a file or stdout.
 *
 *   dvc100-capture | ffplay -
 *   dvc100-capture -c 300 -o clip.y4m
 */
#include "dvc100.h"
#include "recorder.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define QUEUE_LEN 32 /* ~1 s of frames to ride out disk stalls */

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *slots[QUEUE_LEN];
    int head, count;
    int width, height;
    uint64_t dropped;
} frame_queue_t;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* -t: a timer thread instead of alarm(), which Windows lacks */
static void *stop_timer(void *arg)
{
    double left = *(double *)arg;
    while (left > 0 && !g_stop) {
        usleep(50000);
        left -= 0.05;
    }
    g_stop = 1;
    return NULL;
}

static void on_frame(const dvc100_frame_t *f, void *user)
{
    frame_queue_t *q = user;
    pthread_mutex_lock(&q->lock);
    if (q->count == QUEUE_LEN) {
        q->dropped++;
    } else {
        int idx = (q->head + q->count) % QUEUE_LEN;
        memcpy(q->slots[idx], f->data, (size_t)f->width * f->height * 2);
        q->count++;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
}

typedef struct {
    FILE *f;
    uint32_t bytes;
    pthread_mutex_t lock;
} wav_t;

static void wav_header(FILE *f, int rate, uint32_t bytes)
{
    uint8_t h[45] = "RIFF....WAVEfmt \x10\0\0\0\1\0\2\0........\4\0\x10\0data....";
    uint32_t v[] = { 36 + bytes, (uint32_t)rate, (uint32_t)rate * 4, bytes };
    int off[] = { 4, 24, 28, 40 };
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++)
            h[off[i] + b] = (uint8_t)(v[i] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void on_audio(const int16_t *samples, int frames, void *user)
{
    wav_t *w = user;
    pthread_mutex_lock(&w->lock);
    w->bytes += (uint32_t)fwrite(samples, 4, (size_t)frames, w->f) * 4;
    pthread_mutex_unlock(&w->lock);
}

static void write_y4m_frame(FILE *out, const uint8_t *yuyv, int w, int h, uint8_t *planar)
{
    uint8_t *Y = planar, *U = planar + w * h, *V = U + (w / 2) * h;
    for (int i = 0, n = w * h / 2; i < n; i++) {
        Y[2 * i] = yuyv[4 * i];
        U[i] = yuyv[4 * i + 1];
        Y[2 * i + 1] = yuyv[4 * i + 2];
        V[i] = yuyv[4 * i + 3];
    }
    fputs("FRAME\n", out);
    fwrite(planar, 1, (size_t)w * h * 2, out);
}

static void mpeg_frame(const dvc100_frame_t *f, void *user) { rec_video(user, f->data); }
static void mpeg_audio(const int16_t *s, int frames, void *user) { rec_audio(user, s, frames); }

static int record_mpeg2(dvc100_t *dev, const char *path, int w, int h, int pal, int deinterlace,
                        int widescreen, int kbps, long count)
{
    int rate = 48000;
    recorder_opts_t o = { .width = w, .height = h, .pal = pal, .deinterlace = deinterlace, .widescreen = widescreen,
                          .video_kbps = kbps, .audio_rate = rate };
    char err[256];
    recorder_t *rec = rec_open(path, &o, err, sizeof(err));
    if (!rec) {
        fprintf(stderr, "dvc100-capture: %s\n", err);
        dvc100_close(dev);
        return 1;
    }
    int r = dvc100_audio_start(dev, mpeg_audio, rec, &rate);
    if (!r)
        r = dvc100_start(dev, mpeg_frame, rec);
    if (r) {
        fprintf(stderr, "dvc100-capture: cannot start streaming: %s\n", dvc100_strerror(r));
        rec_close(rec);
        dvc100_close(dev);
        return 1;
    }
    fprintf(stderr, "dvc100-capture: recording %s MPEG-2 to %s, Ctrl-C to stop\n",
            deinterlace ? "deinterlaced" : "interlaced", path);
    double limit = count > 0 ? count / (pal ? 25.0 : 30000.0 / 1001) : 0;
    while (!g_stop && (limit <= 0 || rec_seconds(rec) < limit))
        usleep(100000);
    dvc100_stop(dev);
    dvc100_audio_stop(dev);
    double secs = rec_seconds(rec);
    r = rec_close(rec);
    fprintf(stderr, "dvc100-capture: %.1f s recorded%s\n", secs, r ? " (ffmpeg reported an error)" : "");
    dvc100_close(dev);
    return r ? 1 : 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -i, --input composite|svideo   video input (default composite)\n"
            "  -n, --norm auto|ntsc|pal       video standard (default auto)\n"
            "  -f, --format y4m|yuyv          output format (default y4m)\n"
            "  -o, --output FILE              output file, - for stdout (default -)\n"
            "  -c, --count N                  stop after N frames\n"
            "  -m, --mpeg2 FILE.mpg           record video+audio as DVD-compatible MPEG-2\n"
            "  -d, --deinterlace              deinterlace the MPEG-2 recording (default: interlaced)\n"
            "  -w, --widescreen               stretch the MPEG-2 recording to 16:9 (default: 4:3)\n"
            "  -b, --bitrate KBPS             MPEG-2 video bitrate (default 8000)\n"
            "  -a, --audio FILE.wav           also record the audio input (48 kHz stereo)\n"
            "      --audio-only               record audio only (with -a), until Ctrl-C or -t\n"
            "  -t, --time SECONDS             stop after this many seconds\n"
            "      --brightness/--contrast/--saturation/--hue 0..255 (128 = default)\n"
            "      --info                     print device information and exit\n"
            "  -v, --verbose                  more logging (repeat for register traces)\n",
            argv0);
}

int main(int argc, char **argv)
{
    dvc100_input_t input = DVC100_INPUT_COMPOSITE;
    dvc100_norm_t norm = DVC100_NORM_AUTO;
    int raw = 0, info_only = 0, verbose = 0;
    long count = -1;
    const char *outpath = "-", *audiopath = NULL;
    int audio_only = 0, deinterlace = 0, widescreen = 0, kbps = 0;
    const char *mpegpath = NULL;
    double seconds = 0;
    int picture[4] = { -1, -1, -1, -1 };

    enum { OPT_AUDIO_ONLY = 999, OPT_INFO = 1000, OPT_BRI, OPT_CON, OPT_SAT, OPT_HUE };
    static const struct option opts[] = {
        { "input", required_argument, NULL, 'i' },
        { "norm", required_argument, NULL, 'n' },
        { "format", required_argument, NULL, 'f' },
        { "output", required_argument, NULL, 'o' },
        { "count", required_argument, NULL, 'c' },
        { "audio", required_argument, NULL, 'a' },
        { "mpeg2", required_argument, NULL, 'm' },
        { "deinterlace", no_argument, NULL, 'd' },
        { "widescreen", no_argument, NULL, 'w' },
        { "bitrate", required_argument, NULL, 'b' },
        { "audio-only", no_argument, NULL, OPT_AUDIO_ONLY },
        { "time", required_argument, NULL, 't' },
        { "verbose", no_argument, NULL, 'v' },
        { "info", no_argument, NULL, OPT_INFO },
        { "brightness", required_argument, NULL, OPT_BRI },
        { "contrast", required_argument, NULL, OPT_CON },
        { "saturation", required_argument, NULL, OPT_SAT },
        { "hue", required_argument, NULL, OPT_HUE },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:n:f:o:c:a:m:dwb:t:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (!strcmp(optarg, "svideo") || !strcmp(optarg, "s-video"))
                input = DVC100_INPUT_SVIDEO;
            else if (!strcmp(optarg, "composite"))
                input = DVC100_INPUT_COMPOSITE;
            else
                return usage(argv[0]), 2;
            break;
        case 'n':
            if (!strcmp(optarg, "ntsc"))
                norm = DVC100_NORM_NTSC;
            else if (!strcmp(optarg, "pal"))
                norm = DVC100_NORM_PAL;
            else if (!strcmp(optarg, "auto"))
                norm = DVC100_NORM_AUTO;
            else
                return usage(argv[0]), 2;
            break;
        case 'f':
            raw = !strcmp(optarg, "yuyv");
            break;
        case 'o': outpath = optarg; break;
        case 'c': count = strtol(optarg, NULL, 10); break;
        case 'a': audiopath = optarg; break;
        case 'm': mpegpath = optarg; break;
        case 'd': deinterlace = 1; break;
        case 'w': widescreen = 1; break;
        case 'b': kbps = atoi(optarg); break;
        case 't': seconds = atof(optarg); break;
        case OPT_AUDIO_ONLY: audio_only = 1; break;
        case 'v': verbose++; break;
        case OPT_INFO: info_only = 1; break;
        case OPT_BRI: picture[0] = atoi(optarg); break;
        case OPT_CON: picture[1] = atoi(optarg); break;
        case OPT_SAT: picture[2] = atoi(optarg); break;
        case OPT_HUE: picture[3] = atoi(optarg); break;
        default: return usage(argv[0]), c == 'h' ? 0 : 2;
        }
    }
    dvc100_set_verbose(verbose + (info_only ? 1 : 0));

    dvc100_t *dev;
    int r = dvc100_open(&dev, 0, 0);
    if (r) {
        fprintf(stderr, "dvc100-capture: %s\n", dvc100_strerror(r));
        return 1;
    }
    r = dvc100_init(dev, input, norm);
    if (r) {
        fprintf(stderr, "dvc100-capture: init failed: %s\n", dvc100_strerror(r));
        dvc100_close(dev);
        return 1;
    }
    if (picture[0] >= 0) dvc100_set_brightness(dev, picture[0]);
    if (picture[1] >= 0) dvc100_set_contrast(dev, picture[1]);
    if (picture[2] >= 0) dvc100_set_saturation(dev, picture[2]);
    if (picture[3] >= 0) dvc100_set_hue(dev, picture[3]);

    int pal = dvc100_current_norm(dev) == DVC100_NORM_PAL;
    int w = 720, h = pal ? 576 : 480;

    if (info_only) {
        dvc100_info_t info;
        dvc100_get_info(dev, &info);
        printf("em28xx chip id      0x%02x\n", info.chip_id);
        printf("em28xx chip config  0x%02x (%s audio%s)\n", info.chip_cfg,
               info.has_ac97 ? "AC97" : "no AC97", info.vendor_audio ? ", vendor class" : "");
        if (info.integrated_decoder && !info.decoder_addr)
            printf("decoder             built into the bridge (registers 0x7a00)\n");
        else if (info.integrated_decoder)
            printf("decoder             em2828x video decoder at I2C 0x%02x (second bus)\n",
                   info.decoder_addr);
        else
            printf("decoder             SAA711x at I2C 0x%02x, version 0x%02x\n", info.decoder_addr,
                   info.decoder_ver);
        printf("decoder status      0x%02x (%s, %s)\n", info.decoder_status,
               (info.decoder_status & 1) ? "signal locked" : "no signal",
               (info.decoder_status & 0x20) ? "60 Hz" : "50 Hz");
        printf("video standard      %s %dx%d\n", pal ? "PAL" : "NTSC", w, h);
        printf("iso alt setting     %d (%d bytes/microframe)\n", info.alt_setting,
               info.iso_packet_size);
        dvc100_close(dev);
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, on_signal);
#endif
    if (seconds > 0) {
        pthread_t timer;
        if (pthread_create(&timer, NULL, stop_timer, &seconds) == 0)
            pthread_detach(timer);
    }

    if (mpegpath)
        return record_mpeg2(dev, mpegpath, w, h, pal, deinterlace, widescreen, kbps, count);

    wav_t wav = { .f = NULL };
    pthread_mutex_init(&wav.lock, NULL);
    int rate = 48000;
    if (audiopath) {
        wav.f = fopen(audiopath, "wb");
        if (!wav.f) {
            fprintf(stderr, "dvc100-capture: %s: %s\n", audiopath, strerror(errno));
            dvc100_close(dev);
            return 1;
        }
        wav_header(wav.f, rate, 0);
        r = dvc100_audio_start(dev, on_audio, &wav, &rate);
        if (r) {
            fprintf(stderr, "dvc100-capture: cannot start audio: %s\n", dvc100_strerror(r));
            dvc100_close(dev);
            return 1;
        }
    } else if (audio_only) {
        fprintf(stderr, "dvc100-capture: --audio-only needs -a FILE.wav\n");
        dvc100_close(dev);
        return 2;
    }
    if (audio_only) {
        fprintf(stderr, "dvc100-capture: recording audio, Ctrl-C to stop\n");
        while (!g_stop)
            usleep(50000);
        dvc100_audio_stop(dev);
        pthread_mutex_lock(&wav.lock);
        wav_header(wav.f, rate, wav.bytes);
        fclose(wav.f);
        fprintf(stderr, "dvc100-capture: %.1f s of audio written\n", wav.bytes / (rate * 4.0));
        dvc100_close(dev);
        return 0;
    }

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY); /* no CRLF translation of the video */
#endif
    FILE *out = strcmp(outpath, "-") ? fopen(outpath, "wb") : stdout;
    if (!out) {
        fprintf(stderr, "dvc100-capture: %s: %s\n", outpath, strerror(errno));
        dvc100_close(dev);
        return 1;
    }
    if (out == stdout && isatty(STDOUT_FILENO)) {
        fprintf(stderr, "dvc100-capture: refusing to write video to a terminal, use -o or a pipe\n");
        dvc100_close(dev);
        return 2;
    }

    frame_queue_t q = { .width = w, .height = h };
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.cond, NULL);
    for (int i = 0; i < QUEUE_LEN; i++)
        q.slots[i] = malloc((size_t)w * h * 2);
    uint8_t *planar = malloc((size_t)w * h * 2);

    if (!raw)
        fprintf(out, "YUV4MPEG2 W%d H%d %s It %s C422\n", w, h, pal ? "F25:1" : "F30000:1001",
                pal ? "A59:54" : "A10:11");

    r = dvc100_start(dev, on_frame, &q);
    if (r) {
        fprintf(stderr, "dvc100-capture: cannot start streaming: %s\n", dvc100_strerror(r));
        dvc100_close(dev);
        return 1;
    }
    fprintf(stderr, "dvc100-capture: streaming %s %dx%d, Ctrl-C to stop\n", pal ? "PAL" : "NTSC",
            w, h);

    long written = 0;
    uint8_t *frame = malloc((size_t)w * h * 2);
    while (!g_stop && (count < 0 || written < count)) {
        pthread_mutex_lock(&q.lock);
        while (!q.count && !g_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            if (pthread_cond_timedwait(&q.cond, &q.lock, &ts) == ETIMEDOUT) {
                fprintf(stderr, "dvc100-capture: no frames for 2 s (check the video source)\n");
            }
        }
        if (!q.count) {
            pthread_mutex_unlock(&q.lock);
            break;
        }
        memcpy(frame, q.slots[q.head], (size_t)w * h * 2);
        q.head = (q.head + 1) % QUEUE_LEN;
        q.count--;
        pthread_mutex_unlock(&q.lock);

        if (raw)
            fwrite(frame, 1, (size_t)w * h * 2, out);
        else
            write_y4m_frame(out, frame, w, h, planar);
        if (ferror(out))
            break;
        written++;
    }

    dvc100_stop(dev);
    if (wav.f) {
        dvc100_audio_stop(dev);
        pthread_mutex_lock(&wav.lock);
        wav_header(wav.f, rate, wav.bytes);
        fclose(wav.f);
        fprintf(stderr, "dvc100-capture: %.1f s of audio written\n", wav.bytes / (rate * 4.0));
    }
    fflush(out);
    if (out != stdout)
        fclose(out);
    fprintf(stderr, "dvc100-capture: %ld frames written, %llu dropped by writer\n", written,
            (unsigned long long)q.dropped);
    dvc100_close(dev);
    return 0;
}
