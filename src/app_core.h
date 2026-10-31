/*
 * Dazzle Capture engine shared by the Windows and Linux GUIs (the macOS app
 * does the same in Cocoa): device, preview frames, audio monitoring and
 * meters, MPEG-2 recording, snapshots and settings. Everything here is
 * thread safe; the GUI polls it from a timer.
 */
#ifndef DAZZLE_APP_CORE_H
#define DAZZLE_APP_CORE_H

#include "dvc100.h"

#include <stdint.h>

#define APP_NAME "Dazzle Capture"

typedef struct app app_t;

/* Opens and starts the first capture box. On failure returns NULL and puts
 * a sentence for the user in err. */
app_t *app_open(dvc100_input_t input, dvc100_norm_t norm, char *err, int errlen);
void app_close(app_t *a);

int app_width(app_t *a);
int app_height(app_t *a);
int app_is_pal(app_t *a);
void app_set_input(app_t *a, dvc100_input_t input);
void app_set_deinterlace(app_t *a, int on); /* preview line blend; recording uses yadif */
void app_set_widescreen(app_t *a, int on);  /* 16:9 instead of 4:3 for recordings and snapshots */
void app_set_monitor(app_t *a, int on);     /* play the input through the speakers */
const char *app_audio_problem(app_t *a);    /* NULL when audio works */

/* Copies the latest preview frame as 32-bit pixels 0x00RRGGBB (BGRX bytes on
 * little-endian, which is both a Windows DIB and CAIRO_FORMAT_RGB24) into
 * buf (width*height*4) if it is newer than *seq. Returns 1 if copied. */
int app_get_frame(app_t *a, uint32_t *buf, uint64_t *seq);
/* Peak levels since the last call, in dBFS (-60..0). */
void app_peaks(app_t *a, double db[2]);

/* quality: 0 = 6, 1 = 8, 2 = 9.5 Mbit/s. path receives the file name. */
int app_record_start(app_t *a, const char *folder, int quality, int deinterlace, char *path, int pathlen,
                     char *err, int errlen);
/* Stops in the background; done(user, ok, path) runs on a worker thread. */
typedef void (*app_done_cb)(void *user, int ok, const char *path);
void app_record_stop(app_t *a, app_done_cb done, void *user);
int app_is_recording(app_t *a);
double app_record_seconds(app_t *a);

/* Saves the current picture as a square-pixel PNG (640x480 or 768x576). */
int app_snapshot(app_t *a, const char *folder, char *path, int pathlen, char *err, int errlen);

/* "Dazzle Capture 2026-10-30 at 21.05.33.ext" in folder. */
void app_timestamped_path(const char *folder, const char *ext, char *out, int len);

/* Settings: key=value lines in the user's config folder. */
int app_setting_int(const char *key, int def);
void app_setting_str(const char *key, char *out, int len, const char *def);
void app_set_setting_int(const char *key, int v);
void app_set_setting_str(const char *key, const char *v);

/* Default recordings folder: Videos (Windows), XDG videos dir or ~/Videos. */
void app_default_folder(char *out, int len);

/* fopen with a UTF-8 path on every platform. */
#include <stdio.h>
FILE *app_fopen(const char *path, const char *mode);

#endif
