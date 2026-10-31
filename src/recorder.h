/*
 * MPEG-2 program stream recorder (DVD-compatible .mpg) for dvc100 streams.
 * Video and audio are piped to an ffmpeg child process.
 */
#ifndef DVC100_RECORDER_H
#define DVC100_RECORDER_H

#include <stdint.h>

typedef struct recorder recorder_t;

typedef struct {
    int width, height;     /* 720x480 or 720x576 */
    int pal;               /* 25 fps instead of 29.97 */
    int deinterlace;       /* 1 = yadif, encode progressive; 0 = keep interlaced */
    int widescreen;        /* 1 = flag the picture as 16:9 instead of 4:3 */
    int video_kbps;        /* 0 = 8000 */
    int audio_kbps;        /* 0 = 224 */
    int audio_rate;        /* 0 = 48000 */
} recorder_opts_t;

/* Returns NULL and fills err (if given) on failure. */
recorder_t *rec_open(const char *path, const recorder_opts_t *opts, char *err, int errlen);
/* Thread safe, non-blocking: called from the dvc100 frame/audio callbacks. */
void rec_video(recorder_t *r, const uint8_t *yuyv);
void rec_audio(recorder_t *r, const int16_t *samples, int frames);
/* Flushes, waits for ffmpeg; returns 0 on success. */
int rec_close(recorder_t *r);
double rec_seconds(recorder_t *r);
/* Path of the ffmpeg executable used, or NULL if none was found. */
const char *rec_find_ffmpeg(void);

#endif
