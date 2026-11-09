/*
 * Recorder for dvc100 streams: DVD-compatible MPEG-2 (.mpg) by default, or
 * H.264 (.mp4) or DV. Video and audio are piped to an ffmpeg child process.
 */
#ifndef DVC100_RECORDER_H
#define DVC100_RECORDER_H

#include <stdint.h>

typedef struct recorder recorder_t;

/* What the recording is encoded as. MPEG-2 is the default everywhere. */
typedef enum {
    REC_MPEG2 = 0, /* DVD-compatible MPEG-2 + MP2 program stream (.mpg) */
    REC_H264,      /* x264 H.264 + AAC in MP4 (.mp4); needs an ffmpeg with libx264 */
    REC_DV,        /* DV (25 Mbit/s, intra-frame, cheap to encode) + PCM in QuickTime (.mov), for PowerPC Macs */
} rec_codec_t;

/* "mpg", "mp4" or "mov" */
const char *rec_extension(rec_codec_t codec);

typedef struct {
    rec_codec_t codec;
    int width, height;     /* 720x480 or 720x576 */
    int pal;               /* 25 fps instead of 29.97 */
    int deinterlace;       /* 1 = yadif, encode progressive; 0 = keep interlaced */
    int widescreen;        /* 1 = flag the picture as 16:9 instead of 4:3 */
    int video_kbps;        /* MPEG-2: 0 = 8000 */
    int crf;               /* H.264: x264 constant quality, 0 = 20 */
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
