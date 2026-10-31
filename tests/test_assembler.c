/* Feeds synthetic em28xx isochronous packets through the field assembler. */
#include "dvc100.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 720
#define H 480
#define STRIDE (W * 2)
#define PKT 3072

static int frames, failures;
static uint8_t last_frame[W * H * 2];

static void on_frame(const dvc100_frame_t *f, void *user)
{
    (void)user;
    frames++;
    memcpy(last_frame, f->data, sizeof(last_frame));
}

/* pixel value encodes the frame row and frame number */
static uint8_t pattern(int frame, int row, int col) { return (uint8_t)(row * 7 + col + frame * 31); }

static void send_field(dvc100_assembler_t *a, int frame, int top, int seq, int with_88)
{
    size_t field_bytes = (size_t)STRIDE * (H / 2);
    uint8_t *field = malloc(field_bytes);
    for (int line = 0; line < H / 2; line++)
        for (int c = 0; c < STRIDE; c++)
            field[line * STRIDE + c] = pattern(frame, line * 2 + (top ? 0 : 1), c);

    uint8_t pkt[PKT];
    size_t off = 0;
    int first = 1;
    while (off < field_bytes) {
        size_t hdr = 0;
        if (with_88) {
            memset(pkt, 0x88, 4);
            hdr = 4;
        }
        if (first) {
            pkt[hdr] = 0x22;
            pkt[hdr + 1] = 0x5a;
            pkt[hdr + 2] = (uint8_t)((seq << 1) | (top ? 0 : 1));
            pkt[hdr + 3] = 0;
            hdr += 4;
            first = 0;
        }
        size_t n = PKT - hdr;
        if (n > field_bytes - off)
            n = field_bytes - off;
        memcpy(pkt + hdr, field + off, n);
        dvc100_asm_packet(a, pkt, hdr + n);
        off += n;
    }
    free(field);
}

static void check_frame(int frame, const char *what)
{
    for (int row = 0; row < H; row++)
        for (int c = 0; c < STRIDE; c++)
            if (last_frame[row * STRIDE + c] != pattern(frame, row, c)) {
                fprintf(stderr, "FAIL %s: frame %d row %d col %d\n", what, frame, row, c);
                failures++;
                return;
            }
}

int main(void)
{
    dvc100_assembler_t a;
    dvc100_asm_init(&a, W, H, on_frame, NULL);

    /* top/bottom pairs */
    for (int f = 0; f < 3; f++) {
        send_field(&a, f, 1, f * 2, 0);
        send_field(&a, f, 0, f * 2 + 1, 0);
    }
    if (frames != 3) {
        fprintf(stderr, "FAIL: expected 3 frames, got %d\n", frames);
        failures++;
    }
    check_frame(2, "weave");

    /* em2874-style 0x88888888 prefixes are stripped */
    send_field(&a, 5, 1, 0, 1);
    send_field(&a, 5, 0, 1, 1);
    check_frame(5, "0x88 prefix");

    /* a missing bottom field drops the frame instead of emitting garbage */
    int before = frames;
    send_field(&a, 6, 1, 2, 0);
    send_field(&a, 7, 1, 4, 0);
    send_field(&a, 7, 0, 5, 0);
    if (frames != before + 1 || a.dropped != 1) {
        fprintf(stderr, "FAIL: dropped-field handling (frames %d, dropped %llu)\n",
                frames - before, (unsigned long long)a.dropped);
        failures++;
    }
    check_frame(7, "after drop");

    /* VBI packets are ignored */
    uint8_t vbi[64] = { 0x33, 0x95, 0x00, 0x00 };
    dvc100_asm_packet(&a, vbi, sizeof(vbi));
    send_field(&a, 8, 1, 6, 0);
    send_field(&a, 8, 0, 7, 0);
    check_frame(8, "vbi");

    dvc100_asm_free(&a);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("assembler: all tests passed (%d frames)\n", frames);
    return 0;
}
