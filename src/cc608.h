/*
 * Closed captions (EIA-608) from line 21: slices the two bytes out of the captured picture lines and
 * decodes them (CC1-CC4, T1-T4 and XDS) into text lines for a live log.
 *
 * The capture box delivers line 21 of each field as the first lines of the frame once its capture
 * window starts at the top (see dvc100_set_show_line21). cc608_frame() looks at the first few lines of
 * each field, finds the clock run-in on whichever carries it, and feeds the byte pairs to the decoder.
 */
#ifndef DAZZLE_CC608_H
#define DAZZLE_CC608_H

#include <stddef.h>
#include <stdint.h>

typedef struct cc608 cc608_t;

/* line: one UTF-8 text line, no newline ("[CC1] Hello", "XDS: Network: ABC"). Called from the thread that
 * calls cc608_frame/cc608_pair. */
typedef void (*cc608_log_fn)(const char *line, void *user);

cc608_t *cc608_new(cc608_log_fn log, void *user);
void cc608_free(cc608_t *c);

/* One captured frame: YUYV, stride width*2. top_first as in dvc100_frame_t. */
void cc608_frame(cc608_t *c, const uint8_t *yuyv, int width, int height, int top_first);

/* Slices one line of luma samples (one byte per sample, `n` of them, 13.5 MHz). Returns 1 and the two raw
 * bytes (parity bit still on) if it carries a caption signal. */
int cc608_slice(const uint8_t *y, int n, uint8_t out[2]);

/* Feeds one byte pair of a field (0 = first, 1 = second) with the parity bits still on. */
void cc608_pair(cc608_t *c, int field, uint8_t b1, uint8_t b2);

/* Signal statistics, for the status line: byte pairs seen on field 1 and 2. */
void cc608_counts(cc608_t *c, unsigned long *field1, unsigned long *field2);

#endif
