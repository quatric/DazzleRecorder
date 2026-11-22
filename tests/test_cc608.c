/* Closed caption slicer and decoder: synthetic line 21 waveforms through cc608_slice, and byte pairs
 * through the decoder. */
#include "cc608.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static char logbuf[64][256];
static int nlog;
static void on_log(const char *line, void *u)
{
    (void)u;
    if (nlog < 64)
        snprintf(logbuf[nlog++], 256, "%s", line);
}

static uint8_t odd(uint8_t v)
{
    int p = __builtin_popcount(v & 0x7f) & 1;
    return (uint8_t)(v & 0x7f) | (uint8_t)(p ? 0 : 0x80);
}

/* a line 21: black, run-in, 0 0 1, 16 bits, as luma lowpassed like the capture chip does */
static void make_line(uint8_t *y, int n, int start, float T, uint8_t b1, uint8_t b2, int lo, int hi)
{
    float v[768];
    for (int i = 0; i < n; i++)
        v[i] = (float)lo;
    float t0 = (float)start;
    for (int i = 0; i < n; i++) {
        float t = (float)i - t0;
        if (t >= 0 && t < 7 * T)
            v[i] = fmodf(t, T) < T / 2 ? (float)hi : (float)lo; /* 7 cycles, high first */
        else if (t >= 7 * T + 2 * T) { /* start bit 1, then data */
            int bit = (int)((t - 9 * T) / T);
            int val = bit == 0 ? 1 : bit <= 16 ? (((bit - 1) < 8 ? b1 >> (bit - 1) : b2 >> (bit - 9)) & 1) : 0;
            v[i] = (float)(val ? hi : lo);
        }
    }
    for (int pass = 0; pass < 3; pass++) {
        float w[768];
        for (int i = 0; i < n; i++)
            w[i] = (v[i > 0 ? i - 1 : 0] + 2 * v[i] + v[i < n - 1 ? i + 1 : n - 1]) / 4;
        memcpy(v, w, sizeof(float) * (size_t)n);
    }
    for (int i = 0; i < n; i++)
        y[i] = (uint8_t)(v[i] + 0.5f);
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void pair(cc608_t *c, int field, uint8_t a, uint8_t b) { cc608_pair(c, field, odd(a), odd(b)); }
static void text(cc608_t *c, int field, const char *s)
{
    for (; s[0]; s += s[1] ? 2 : 1)
        pair(c, field, (uint8_t)s[0], s[1] ? (uint8_t)s[1] : 0);
}

int main(void)
{
    uint8_t y[720], out[2];
    struct { int start; float T; int lo, hi; } cases[] = { { 18, 26.81f, 16, 120 }, { 30, 26.81f, 20, 110 },
                                                           { 12, 26.9f, 16, 90 }, { 25, 26.7f, 40, 150 } };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (int k = 0; k < 4; k++) {
            uint8_t b1 = odd((uint8_t)(0x20 + k * 17)), b2 = odd((uint8_t)(0x41 + i * 9));
            make_line(y, 720, cases[i].start, cases[i].T, b1, b2, cases[i].lo, cases[i].hi);
            int ok = cc608_slice(y, 720, out);
            CHECK(ok && out[0] == b1 && out[1] == b2);
        }
    }
    /* null pair 0x80 0x80 */
    make_line(y, 720, 18, 26.81f, 0x80, 0x80, 16, 120);
    CHECK(cc608_slice(y, 720, out) && out[0] == 0x80 && out[1] == 0x80);
    /* flat line and video-like noise carry no caption */
    memset(y, 16, sizeof(y));
    CHECK(!cc608_slice(y, 720, out));
    for (int i = 0; i < 720; i++)
        y[i] = (uint8_t)(60 + 50 * sinf((float)i * 0.013f) + (i * 7919 % 13));
    CHECK(!cc608_slice(y, 720, out));

    /* pop-on caption on CC1 */
    cc608_t *c = cc608_new(on_log, NULL);
    pair(c, 0, 0x14, 0x20); pair(c, 0, 0x14, 0x20);        /* RCL, sent twice */
    pair(c, 0, 0x14, 0x70); pair(c, 0, 0x14, 0x70);        /* PAC row 15, indent 0 */
    text(c, 0, "Hello, world");
    pair(c, 0, 0x14, 0x2f); pair(c, 0, 0x14, 0x2f);        /* EOC */
    CHECK(nlog == 1 && !strcmp(logbuf[0], "[CC1] Hello, world"));

    /* roll-up on CC2: a line is logged on CR */
    nlog = 0;
    pair(c, 0, 0x1c, 0x25); pair(c, 0, 0x1c, 0x25);        /* RU2 on CC2 */
    text(c, 0, "ab");
    pair(c, 0, 0x1c, 0x2d); pair(c, 0, 0x1c, 0x2d);        /* CR */
    CHECK(nlog == 1 && !strcmp(logbuf[0], "[CC2] ab"));

    /* special character */
    nlog = 0;
    pair(c, 0, 0x14, 0x25); pair(c, 0, 0x14, 0x25);
    pair(c, 0, 0x11, 0x37); text(c, 0, "x");
    pair(c, 0, 0x14, 0x2d);
    CHECK(nlog == 1 && !strcmp(logbuf[0], "[CC1] \xe2\x99\xaa" "x"));

    /* XDS: network name "ABC" on field 2 with its checksum */
    nlog = 0;
    {
        uint8_t d[] = { 0x05, 0x01, 'A', 'B', 'C', 0x00, 0x0f };
        int sum = 0;
        for (unsigned i = 0; i < sizeof(d); i++)
            sum += d[i];
        uint8_t ck = (uint8_t)((128 - (sum & 0x7f)) & 0x7f);
        pair(c, 1, 0x05, 0x01);
        pair(c, 1, 'A', 'B');
        pair(c, 1, 'C', 0x00);
        pair(c, 1, 0x0f, ck);
        CHECK(nlog == 1 && !strcmp(logbuf[0], "XDS: Network: ABC"));
        /* the same again is not logged twice */
        pair(c, 1, 0x05, 0x01); pair(c, 1, 'A', 'B'); pair(c, 1, 'C', 0x00); pair(c, 1, 0x0f, ck);
        CHECK(nlog == 1);
    }
    cc608_free(c);
    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("cc608: all tests passed\n");
    return 0;
}
