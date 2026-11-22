/* EIA-608 closed captions and XDS from the captured line 21. See cc608.h.
 *
 * The slicer (run-in correlation, bit sampling, single-bit parity repair) and the XDS descriptions follow
 * eshaz/cc_decoder (https://github.com/eshaz/cc_decoder, from CordySmith/cc_decoder by Max Smith; The
 * Unlicense), rewritten in C. */
#include "cc608.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- slicer ---- */

#define MAXN 768
#define MAXW 24
#define MAXT 320
#define RUNIN 6.5     /* cycles of the clock run-in matched (the real one has 7) */
#define MIN_SCORE 0.5 /* minimum normalized correlation of the run-in and start bits */

/* Templates of the run-in and the three start bits (0 0 1) for each bit width searched; the width is a
 * fraction of the line the way eshaz/cc_decoder does it (35..41 thousandths of the line per bit). */
typedef struct {
    int n, nw;
    float w[MAXW];
    int run[MAXW], len[MAXW], total[MAXW];
    double var[MAXW];
    float t[MAXW][MAXT];
} tset_t;

static void tset_build(tset_t *s, int n)
{
    memset(s, 0, sizeof(*s));
    s->n = n;
    int lo = (int)floor(0.035 * n + 0.5), hi = (int)floor(0.041 * n + 0.5);
    int nw = (hi - lo) * 4 + 1;
    if (nw < 1)
        nw = 1;
    if (nw > MAXW)
        nw = MAXW;
    for (int i = 0; i < nw; i++) {
        float w = nw > 1 ? (float)lo + (float)(hi - lo) * (float)i / (float)(nw - 1) : (float)lo;
        int run = (int)floor(RUNIN * w + 0.5), zeros = (int)floor(2 * w + 0.5), ones = (int)floor(w + 0.5);
        int len = run + zeros + ones;
        if (len > MAXT)
            break;
        double mean = 0;
        for (int k = 0; k < len; k++) {
            float v = k < run ? 0.5f + 0.5f * (float)cos(2 * M_PI * k / w) : k < run + zeros ? 0.f : 1.f;
            s->t[i][k] = v;
            mean += v;
        }
        mean /= len;
        double var = 0;
        for (int k = 0; k < len; k++) {
            s->t[i][k] -= (float)mean;
            var += (double)s->t[i][k] * s->t[i][k];
        }
        s->w[i] = w;
        s->run[i] = run;
        s->len[i] = len;
        s->total[i] = (int)floor((RUNIN + 3 + 16) * w + 0.5);
        s->var[i] = var;
        s->nw = i + 1;
    }
}

/* One bit: mean and standard deviation of the middle 80% of its cell; 1 when above the threshold. */
static int get_bit(const float *x, int n, float w, int pad, float pre_end, int idx, float thr, float *sd)
{
    float start = pre_end + (float)idx * w;
    int s = (int)floor(start + 0.5f) + pad, e = (int)floor(start + w + 0.5f) - pad;
    if (s < 0)
        s = 0;
    if (e > n)
        e = n;
    if (e <= s) {
        *sd = 1;
        return 0;
    }
    double m = 0, v = 0;
    for (int i = s; i < e; i++)
        m += x[i];
    m /= e - s;
    for (int i = s; i < e; i++)
        v += (x[i] - m) * (x[i] - m);
    *sd = (float)sqrt(v / (e - s));
    return m > thr;
}

static int slice_with(tset_t *ts, const uint8_t *y, int n, uint8_t out[2])
{
    if (n < 400 || n > MAXN)
        return 0;
    if (ts->n != n)
        tset_build(ts, n);

    /* normalize the line between its 2nd and 98th percentile */
    int hist[256] = { 0 };
    for (int i = 0; i < n; i++)
        hist[y[i]]++;
    int lo = 0, hi = 255, acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc * 100 >= n * 2) {
            lo = v;
            break;
        }
    }
    acc = 0;
    for (int v = 255; v >= 0; v--) {
        acc += hist[v];
        if (acc * 100 >= n * 2) {
            hi = v;
            break;
        }
    }
    if (hi - lo < 24)
        return 0;
    float x[MAXN];
    for (int i = 0; i < n; i++) {
        float v = (float)(y[i] - lo) / (float)(hi - lo);
        x[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    double cs[MAXN + 1], cs2[MAXN + 1];
    cs[0] = cs2[0] = 0;
    for (int i = 0; i < n; i++) {
        cs[i + 1] = cs[i] + x[i];
        cs2[i + 1] = cs2[i] + (double)x[i] * x[i];
    }

    /* the clock run-in and start bits: best normalized correlation over the bit widths and the positions where
     * the whole 19 bits after the run-in still fit on the line */
    double best = -1;
    int bpos = -1, bw = -1;
    for (int wi = 0; wi < ts->nw; wi++) {
        const int len = ts->len[wi], last = n - ts->total[wi];
        const float *t = ts->t[wi];
        for (int p = 0; p < last; p++) {
            double dot = 0;
            for (int k = 0; k < len; k++)
                dot += (double)x[p + k] * t[k];
            double sx = cs[p + len] - cs[p], sx2 = cs2[p + len] - cs2[p];
            double vx = sx2 - sx * sx / len;
            double score = dot * dot / (ts->var[wi] * vx + 1e-12);
            if (score > best) {
                best = score;
                bpos = p;
                bw = wi;
            }
        }
    }
    if (bw < 0 || best < MIN_SCORE)
        return 0;

    const float w = ts->w[bw];
    const float pre_start = (float)bpos, pre_end = (float)(bpos + ts->run[bw]);
    const int pad = (int)ceil(0.1 * w);
    float thr = 0;
    {
        int a = (int)floor(pre_start + 0.5f), b = (int)floor(pre_end + 0.5f);
        double m = 0;
        for (int i = a; i < b; i++)
            m += x[i];
        thr = (float)(m / (b - a));
    }
    float sd;
    if (get_bit(x, n, w, pad, pre_end, 0, thr, &sd) != 0 || get_bit(x, n, w, pad, pre_end, 1, thr, &sd) != 0 ||
        get_bit(x, n, w, pad, pre_end, 2, thr, &sd) != 1)
        return 0;

    /* two bytes: 7 data bits LSB first, then the parity bit; a single doubtful data bit is repaired by the
     * parity when the parity bit itself reads cleanly */
    for (int b = 0; b < 2; b++) {
        const int first = 3 + b * 8;
        int bits[8], err = 0, worst = 0, ones = 0;
        float sds[8], worst_sd = 0;
        for (int i = 0; i < 8; i++) {
            bits[i] = get_bit(x, n, w, pad, pre_end, first + i, thr, &sds[i]);
            if (i < 7) {
                ones += bits[i];
                if (sds[i] > 0.3f) {
                    err++;
                    if (sds[i] > worst_sd) {
                        worst_sd = sds[i];
                        worst = i;
                    }
                }
            }
        }
        const int want = (ones + 1) & 1; /* the parity bit that gives odd parity overall */
        if (err == 1 && bits[7] != want && sds[7] < 0.3f)
            bits[worst] ^= 1;
        int v = 0;
        for (int i = 0; i < 8; i++)
            v |= bits[i] << i;
        out[b] = (uint8_t)v;
    }
    return 1;
}

int cc608_slice(const uint8_t *y, int n, uint8_t out[2])
{
    static tset_t shared; /* only the capture thread slices */
    return slice_with(&shared, y, n, out);
}

/* ---- text cells ---- */

typedef uint32_t cell_t;
#define ROWS 15
#define COLS 33

typedef struct {
    cell_t disp[ROWS + 1][COLS + 1], non[ROWS + 1][COLS + 1]; /* rows 1..15 */
    int mode; /* 0 unknown, 1 pop-on, 2 roll-up, 3 paint-on, 4 text */
    int row, col, rollup;
} chan_t;

#define XDS_CLASSES 8
#define XDS_MAX 40

struct cc608 {
    cc608_log_fn log;
    void *user;
    chan_t cc[4], tx[4];
    int text[4];      /* the channel's data goes to its text service */
    int active[2];    /* per field: the channel number the data bytes belong to */
    uint8_t last[2][2];
    int have_last[2];
    tset_t tset;
    int pref[2], miss[2];
    unsigned long count[2];
    /* XDS */
    int xcur, xclass[XDS_CLASSES], xtype[XDS_CLASSES], xlen[XDS_CLASSES];
    int xsum[XDS_CLASSES];
    uint8_t xdata[XDS_CLASSES][XDS_MAX];
    char xlast[XDS_CLASSES][128][48];
};

cc608_t *cc608_new(cc608_log_fn log, void *user)
{
    cc608_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->log = log;
    c->user = user;
    c->pref[0] = c->pref[1] = -1;
    c->xcur = -1;
    c->active[0] = 0;
    c->active[1] = 2;
    return c;
}

void cc608_free(cc608_t *c) { free(c); }

void cc608_counts(cc608_t *c, unsigned long *f1, unsigned long *f2)
{
    if (f1)
        *f1 = c->count[0];
    if (f2)
        *f2 = c->count[1];
}

static void emit(cc608_t *c, const char *s)
{
    if (c->log)
        c->log(s, c->user);
}

static char *utf8(char *p, cell_t u)
{
    if (u < 0x80)
        *p++ = (char)u;
    else if (u < 0x800) {
        *p++ = (char)(0xc0 | u >> 6);
        *p++ = (char)(0x80 | (u & 0x3f));
    } else {
        *p++ = (char)(0xe0 | u >> 12);
        *p++ = (char)(0x80 | (u >> 6 & 0x3f));
        *p++ = (char)(0x80 | (u & 0x3f));
    }
    return p;
}

/* a row as trimmed UTF-8; returns its length (0 = blank) */
static int row_text(const cell_t *r, char *out)
{
    int a = 1, b = COLS - 1;
    while (a <= b && (!r[a] || r[a] == ' '))
        a++;
    while (b >= a && (!r[b] || r[b] == ' '))
        b--;
    char *p = out;
    for (int i = a; i <= b; i++)
        p = utf8(p, r[i] ? r[i] : ' ');
    *p = 0;
    return (int)(p - out);
}

/* Logs the rows of a screen as one caption: "[CC1] first row / second row". */
static void emit_rows(cc608_t *c, const char *tag, int first, int last, cell_t (*mem)[COLS + 1])
{
    char line[ROWS * 140 + 16], one[COLS * 3 + 4];
    int n = snprintf(line, sizeof(line), "[%s] ", tag), any = 0;
    for (int r = first; r <= last; r++)
        if (row_text(mem[r], one)) {
            if (any)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " / ");
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s", one);
            any = 1;
        }
    if (any)
        emit(c, line);
}

static void tag_of(cc608_t *c, int idx, char *tag)
{
    snprintf(tag, 8, "%s%d", c->text[idx] ? "T" : "CC", idx + 1);
}

static chan_t *cur(cc608_t *c, int idx) { return c->text[idx] ? &c->tx[idx] : &c->cc[idx]; }

static void clear_mem(cell_t (*m)[COLS + 1]) { memset(m, 0, sizeof(cell_t) * (ROWS + 1) * (COLS + 1)); }

static void put(cc608_t *c, int idx, cell_t ch)
{
    chan_t *h = cur(c, idx);
    if (!h->mode) {
        h->mode = 2;
        h->rollup = 4;
        h->row = ROWS;
        h->col = 1;
    }
    if (h->row < 1)
        h->row = 1;
    if (h->col < 1)
        h->col = 1;
    if (h->col > COLS - 1)
        h->col = COLS - 1; /* the last column keeps being overwritten, as on a real decoder */
    cell_t(*m)[COLS + 1] = h->mode == 1 ? h->non : h->disp;
    m[h->row][h->col] = ch;
    if (h->col < COLS - 1)
        h->col++;
}

static cell_t basic(int b)
{
    switch (b) {
    case 0x2a: return 0xe1; /* á */
    case 0x5c: return 0xe9; /* é */
    case 0x5e: return 0xed; /* í */
    case 0x5f: return 0xf3; /* ó */
    case 0x60: return 0xfa; /* ú */
    case 0x7b: return 0xe7; /* ç */
    case 0x7c: return 0xf7; /* ÷ */
    case 0x7d: return 0xd1; /* Ñ */
    case 0x7e: return 0xf1; /* ñ */
    case 0x7f: return 0x2588; /* █ */
    }
    return (cell_t)b;
}

static const cell_t special_map[16] = { 0xae, 0xb0, 0xbd, 0xbf, 0x2122, 0xa2, 0xa3, 0x266a,
                                        0xe0, ' ',  0xe8, 0xe2, 0xea,   0xee, 0xf4, 0xfb };
static const cell_t ext12[32] = { 0xc1, 0xc9, 0xd3, 0xda, 0xdc, 0xfc, 0x2018, 0xa1, '*', 0x2019, 0x2014,
                                  0xa9, 0x2120, 0x2022, 0x201c, 0x201d, 0xc0, 0xc2, 0xc7, 0xc8, 0xca,
                                  0xcb, 0xeb, 0xce, 0xcf, 0xef, 0xd4, 0xd9, 0xf9, 0xdb, 0xab, 0xbb };
static const cell_t ext13[32] = { 0xc3, 0xe3, 0xcd, 0xcc, 0xec, 0xd2, 0xf2, 0xd5, 0xf5, '{', '}',
                                  '\\', '^', '_', '|', '~', 0xc4, 0xe4, 0xd6, 0xf6, 0xdf,
                                  0xa5, 0xa4, 0x2502, 0xc5, 0xe5, 0xd8, 0xf8, 0x250c, 0x2510, 0x2514, 0x2518 };

static void shift_up(chan_t *h)
{
    int top = h->row - h->rollup + 1;
    if (top < 1)
        top = 1;
    for (int r = top; r < h->row; r++)
        memcpy(h->disp[r], h->disp[r + 1], sizeof(h->disp[r]));
    memset(h->disp[h->row], 0, sizeof(h->disp[h->row]));
}

static void misc_control(cc608_t *c, int idx, int b2)
{
    char tag[8], one[COLS * 3 + 4];
    if (b2 == 0x20 || b2 == 0x29 || b2 == 0x2a || b2 == 0x2b || (b2 >= 0x25 && b2 <= 0x27)) {
        /* the service this code belongs to */
        c->text[idx] = b2 == 0x2a || b2 == 0x2b;
    }
    chan_t *h = cur(c, idx);
    tag_of(c, idx, tag);
    switch (b2) {
    case 0x20: /* RCL: resume caption loading */
        h->mode = 1;
        break;
    case 0x21: /* BS */
        if (h->col > 1) {
            h->col--;
            (h->mode == 1 ? h->non : h->disp)[h->row][h->col] = 0;
        }
        break;
    case 0x24: /* DER */
        for (int i = h->col; i < COLS; i++)
            (h->mode == 1 ? h->non : h->disp)[h->row][i] = 0;
        break;
    case 0x25:
    case 0x26:
    case 0x27: /* RU2..RU4 */
        if (h->mode != 2)
            clear_mem(h->disp);
        h->mode = 2;
        h->rollup = b2 - 0x23;
        h->row = ROWS;
        h->col = 1;
        break;
    case 0x29: /* RDC: paint-on */
        h->mode = 3;
        break;
    case 0x2a: /* TR: text restart */
        clear_mem(h->disp);
        h->mode = 4;
        h->row = 1;
        h->col = 1;
        break;
    case 0x2b: /* RTD */
        if (h->mode != 4) {
            h->mode = 4;
            if (h->row < 1)
                h->row = 1;
        }
        break;
    case 0x2c: /* EDM: erase displayed memory */
        if (h->mode == 3)
            emit_rows(c, tag, 1, ROWS, h->disp);
        else if (h->mode == 2 && row_text(h->disp[h->row], one)) {
            char line[COLS * 3 + 16];
            snprintf(line, sizeof(line), "[%s] %s", tag, one);
            emit(c, line);
        }
        clear_mem(h->disp);
        break;
    case 0x2d: /* CR */
        if (h->mode == 2) {
            if (row_text(h->disp[h->row], one)) {
                char line[COLS * 3 + 16];
                snprintf(line, sizeof(line), "[%s] %s", tag, one);
                emit(c, line);
            }
            shift_up(h);
            h->col = 1;
        } else if (h->mode == 4) {
            if (row_text(h->disp[h->row], one)) {
                char line[COLS * 3 + 16];
                snprintf(line, sizeof(line), "[%s] %s", tag, one);
                emit(c, line);
            }
            memset(h->disp[h->row], 0, sizeof(h->disp[h->row]));
            h->row = h->row >= ROWS ? 1 : h->row + 1;
            h->col = 1;
        }
        break;
    case 0x2e: /* ENM */
        clear_mem(h->non);
        break;
    case 0x2f: /* EOC: show what was loaded */
        c->text[idx] = 0;
        h = &c->cc[idx];
        {
            cell_t tmp[ROWS + 1][COLS + 1];
            memcpy(tmp, h->disp, sizeof(tmp));
            memcpy(h->disp, h->non, sizeof(tmp));
            memcpy(h->non, tmp, sizeof(tmp));
        }
        h->mode = 1;
        emit_rows(c, tag, 1, ROWS, h->disp);
        break;
    default:
        break;
    }
}

static const int pac_row[8][2] = { { 11, 11 }, { 1, 2 }, { 3, 4 }, { 12, 13 }, { 14, 15 }, { 5, 6 }, { 7, 8 }, { 9, 10 } };

static void pac(cc608_t *c, int idx, int b1, int b2)
{
    chan_t *h = cur(c, idx);
    int row = pac_row[b1 & 7][b2 >> 5 & 1];
    if (!h->mode) {
        h->mode = 2;
        h->rollup = 4;
    }
    if (h->mode == 2) {
        /* a roll-up keeps writing on its base row; the PAC only moves the base up when it asks for a row
         * above the current one */
        if (row < h->rollup)
            row = h->rollup;
        if (h->row != row && h->row >= 1) {
            /* move the rolled rows to the new base */
            cell_t tmp[ROWS + 1][COLS + 1];
            memset(tmp, 0, sizeof(tmp));
            int n = h->rollup;
            for (int i = 0; i < n; i++) {
                int from = h->row - i, to = row - i;
                if (from >= 1 && to >= 1)
                    memcpy(tmp[to], h->disp[from], sizeof(tmp[to]));
            }
            memcpy(h->disp, tmp, sizeof(tmp));
        }
    }
    h->row = row;
    h->col = 1;
    if (b2 & 0x10)
        h->col += (b2 >> 1 & 7) * 4;
}

static void control(cc608_t *c, int field, int b1, int b2)
{
    int idx = field * 2 + (b1 >> 3 & 1);
    c->active[field] = idx;
    int base = b1 & 0x17;
    if (b2 >= 0x40) {
        pac(c, idx, base, b2);
        return;
    }
    switch (base) {
    case 0x11:
        if (b2 >= 0x20 && b2 <= 0x2f)
            put(c, idx, ' '); /* mid-row code: colour or style, shown as the space it occupies */
        else if (b2 >= 0x30 && b2 <= 0x3f)
            put(c, idx, special_map[b2 - 0x30]);
        break;
    case 0x12:
    case 0x13:
        if (b2 >= 0x20 && b2 <= 0x3f) {
            chan_t *h = cur(c, idx);
            if (h->col > 1) /* it replaces the basic character sent before it */
                h->col--;
            put(c, idx, (base == 0x12 ? ext12 : ext13)[b2 - 0x20]);
        }
        break;
    case 0x14:
    case 0x15:
        if (b2 >= 0x20 && b2 <= 0x2f)
            misc_control(c, idx, b2);
        break;
    case 0x17:
        if (b2 >= 0x21 && b2 <= 0x23) {
            chan_t *h = cur(c, idx);
            h->col += b2 - 0x20;
        }
        break;
    default:
        break; /* background and foreground attributes */
    }
}

/* ---- XDS ---- */

static const char *const ptype[96] = {
    "Education", "Entertainment", "Movie", "News", "Religious", "Sports", "Other", "Action", "Advertisement",
    "Animated", "Anthology", "Automobile", "Awards", "Baseball", "Basketball", "Bulletin", "Business",
    "Classical", "College", "Combat", "Comedy", "Commentary", "Concert", "Consumer", "Contemporary", "Crime",
    "Dance", "Documentary", "Drama", "Elementary", "Erotica", "Exercise", "Fantasy", "Farm", "Fashion",
    "Fiction", "Food", "Football", "Foreign", "Fund Raiser", "Game/Quiz", "Garden", "Golf", "Government",
    "Health", "High School", "History", "Hobby", "Hockey", "Home", "Horror", "Information", "Instruction",
    "International", "Interview", "Language", "Legal", "Live", "Local", "Math", "Medical", "Meeting",
    "Military", "Miniseries", "Music", "Mystery", "National", "Nature", "Police", "Politics", "Premiere",
    "Prerecorded", "Product", "Professional", "Public", "Racing", "Reading", "Repair", "Repeat", "Review",
    "Romance", "Science", "Series", "Service", "Shopping", "Soap Opera", "Special", "Suspense", "Talk",
    "Technical", "Tennis", "Travel", "Variety", "Video", "Weather", "Western"
};

static void xds_log(cc608_t *c, int cls, int type, const char *s)
{
    if (type < 0 || type > 127)
        return;
    char *last = c->xlast[cls][type];
    if (!strncmp(last, s, sizeof(c->xlast[0][0]) - 1) && last[0])
        return; /* sent again unchanged: the broadcaster repeats these every few seconds */
    snprintf(last, sizeof(c->xlast[0][0]), "%s", s);
    char line[200];
    snprintf(line, sizeof(line), "XDS: %s", s);
    emit(c, line);
}

static void xds_text(const uint8_t *d, int n, char *out, size_t cap)
{
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++)
        if (d[i] >= 0x20 && d[i] < 0x7f)
            out[k++] = (char)d[i];
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static const char *const lang[8] = { "Unknown", "English", "Spanish", "French", "German", "Italian", "Other", "None" };
static const char *const atype_main[8] = { "Unknown", "Mono", "Simulated stereo", "Stereo", "Stereo surround",
                                           "Data service", "Other", "None" };
static const char *const atype_sap[8] = { "Unknown", "Mono", "Video descriptions", "Non-program audio",
                                          "Special effects", "Data service", "Other", "None" };
static const char *const cgms[4] = { "copying permitted", "no more copies", "one generation of copies",
                                     "no copying permitted" };
static const char *const aps[4] = { "no analog protection", "PSP on", "PSP on, 2-line split burst",
                                    "PSP on, 4-line split burst" };
static const char *const dow[8] = { "?", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon[13] = { "?", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
                                     "Nov", "Dec" };

/* Content advisory (CEA-608-E 9.5.1.5): the rating system is in a1 a0 (and a3 a2 for the Canadian ones) */
static void advisory(const uint8_t *d, int n, char *s, size_t cap)
{
    static const char *const mpa[8] = { "N/A", "G", "PG", "PG-13", "R", "NC-17", "X", "Not rated" };
    static const char *const tv[8] = { "None", "TV-Y", "TV-Y7", "TV-G", "TV-PG", "TV-14", "TV-MA", "None" };
    static const char *const cen[8] = { "E", "C", "C8+", "G", "PG", "14+", "18+", "Invalid" };
    static const char *const cfr[8] = { "E", "G", "8 ans +", "13 ans +", "16 ans +", "18 ans +", "Invalid", "Invalid" };
    if (n < 2) {
        snprintf(s, cap, "Rating: ?");
        return;
    }
    const int c1 = d[0] & 0x7f, c2 = d[1] & 0x7f, a0 = (c1 >> 3) & 1, a1 = (c1 >> 4) & 1;
    if (!a0) { /* MPA, system 0 and 2 */
        snprintf(s, cap, "Rating: MPA %s", mpa[c1 & 7]);
    } else if (!a1) { /* U.S. TV Parental Guidelines */
        int k = snprintf(s, cap, "Rating: %s", tv[c2 & 7]);
        int g = c2 & 7;
        if (g == 2 && (c2 & 0x20))
            k += snprintf(s + k, cap - (size_t)k, " Fantasy Violence");
        if (g == 4 || g == 5 || g == 6) {
            if (c2 & 0x20)
                k += snprintf(s + k, cap - (size_t)k, " Violence");
            if (c2 & 0x10)
                k += snprintf(s + k, cap - (size_t)k, " Sexual Situations");
            if (c2 & 0x08)
                k += snprintf(s + k, cap - (size_t)k, " Adult Language");
            if ((g == 4 || g == 5) && (c1 & 0x20))
                snprintf(s + k, cap - (size_t)k, " Suggestive Dialogue");
        }
    } else { /* a1 a0 = 1 1: a3 a2 pick the Canadian systems */
        const int a2 = (c1 >> 5) & 1, a3 = (c2 >> 3) & 1;
        if (!a3 && !a2)
            snprintf(s, cap, "Rating: Canada %s", cen[c2 & 7]);
        else if (!a3 && a2)
            snprintf(s, cap, "Rating: Canada (French) %s", cfr[c2 & 7]);
        else
            snprintf(s, cap, "Rating (other system): %02x %02x", c1, c2);
    }
}

/* Information in the program packets that both the current and the next program carry */
static int program_info(cc608_t *c, int slot, int type, const uint8_t *d, int n, const char *pre)
{
    char s[160], t[80];
    switch (type) {
    case 0x05:
        advisory(d, n, t, sizeof(t));
        snprintf(s, sizeof(s), "%s%s", pre, t);
        xds_log(c, slot, type, s);
        return 1;
    case 0x06:
        if (n >= 2)
            snprintf(s, sizeof(s), "%sAudio: main %s (%s), second %s (%s)", pre, lang[(d[0] >> 3) & 7],
                     atype_main[d[0] & 7], lang[(d[1] >> 3) & 7], atype_sap[d[1] & 7]);
        else
            return 1;
        xds_log(c, slot, type, s);
        return 1;
    case 0x07: {
        int k = snprintf(s, sizeof(s), "%sCaption services:", pre);
        for (int i = 0; i < n && i < 8; i++) {
            int b = d[i];
            k += snprintf(s + k, sizeof(s) - (size_t)k, "%s %s F%d C%d %s", i ? "," : "", lang[(b >> 3) & 7],
                          1 + ((b >> 2) & 1), 1 + ((b >> 1) & 1), (b & 1) ? "Text" : "Caption");
            if (k >= (int)sizeof(s) - 40)
                break;
        }
        xds_log(c, slot, type, s);
        return 1;
    }
    case 0x08:
        if (n >= 1) {
            snprintf(s, sizeof(s), "%sCopy control: %s, %s%s", pre, cgms[(d[0] >> 3) & 3], aps[(d[0] >> 1) & 3],
                     (n >= 2 && (d[1] & 1)) ? ", redistribution controlled" : "");
            xds_log(c, slot, type, s);
        }
        return 1;
    case 0x09:
        if (n >= 2) {
            snprintf(s, sizeof(s), "%sAspect ratio: lines %d to %d%s", pre, 22 + (d[0] & 0x3f), 262 - (d[1] & 0x3f),
                     (n >= 3 && (d[2] & 1)) ? ", anamorphic" : "");
            xds_log(c, slot, type, s);
        }
        return 1;
    default:
        if (type >= 0x10 && type <= 0x17) {
            xds_text(d, n, t, sizeof(t));
            snprintf(s, sizeof(s), "%sProgram description %d: %s", pre, type - 0x0f, t);
            xds_log(c, slot, type, s);
            return 1;
        }
    }
    return 0;
}

static void xds_packet(cc608_t *c, int slot)
{
    const uint8_t *d = c->xdata[slot];
    const int n = c->xlen[slot], type = c->xtype[slot];
    char s[160], t[80];
    const char *pre = slot == 2 ? "Next: " : "";
    if (slot == 1 || slot == 2) { /* current / future program */
        switch (type) {
        case 0x01:
            if (n >= 4 && (d[1] & 0x1f) < 24 && (d[0] & 0x3f) < 60 && (d[3] & 0x0f) >= 1 && (d[3] & 0x0f) <= 12) {
                snprintf(s, sizeof(s), "%sProgram start %02d:%02d on %d/%d%s", pre, d[1] & 0x1f, d[0] & 0x3f,
                         d[3] & 0x0f, d[2] & 0x1f, (d[3] & 0x10) ? " (tape delayed)" : "");
                xds_log(c, slot, type, s);
            }
            return;
        case 0x02:
            if (n >= 2) {
                int len = (d[1] & 0x3f) * 60 + (d[0] & 0x3f);
                int off = 0, el = -1, sec = -1;
                if (n >= 4)
                    el = (d[3] & 0x3f) * 60 + (d[2] & 0x3f);
                if (n >= 6)
                    sec = d[4] & 0x3f;
                off = snprintf(s, sizeof(s), "%sProgram length %d:%02d", pre, len / 60, len % 60);
                if (el >= 0)
                    off += snprintf(s + off, sizeof(s) - (size_t)off, ", elapsed %d:%02d", el / 60, el % 60);
                if (sec >= 0)
                    snprintf(s + off, sizeof(s) - (size_t)off, ":%02d", sec);
                /* the elapsed time changes every minute: keep it out of the repeat check */
                xds_log(c, slot, type, s);
            }
            return;
        case 0x03:
            xds_text(d, n, t, sizeof(t));
            snprintf(s, sizeof(s), "%sProgram name: %s", pre, t);
            xds_log(c, slot, type, s);
            return;
        case 0x04: {
            int k = snprintf(s, sizeof(s), "%sProgram type: ", pre);
            for (int i = 0; i < n; i++) {
                int b = d[i] & 0x7f;
                if (b >= 0x20 && b < 0x80)
                    k += snprintf(s + k, sizeof(s) - (size_t)k, "%s%s", i ? ", " : "", ptype[b - 0x20]);
            }
            xds_log(c, slot, type, s);
            return;
        }
        default:
            if (program_info(c, slot, type, d, n, pre))
                return;
            break;
        }
    } else if (slot == 3) { /* channel */
        switch (type) {
        case 0x01:
            xds_text(d, n, t, sizeof(t));
            snprintf(s, sizeof(s), "Network: %s", t);
            xds_log(c, slot, type, s);
            return;
        case 0x02:
            xds_text(d, n, t, sizeof(t));
            snprintf(s, sizeof(s), "Station: %s", t);
            xds_log(c, slot, type, s);
            return;
        case 0x03:
            if (n >= 2) {
                int m = (d[1] & 0x1f) * 60 + (d[0] & 0x3f);
                snprintf(s, sizeof(s), "Tape delay: %d:%02d", m / 60, m % 60);
                xds_log(c, slot, type, s);
            }
            return;
        case 0x04:
            if (n >= 4) {
                snprintf(s, sizeof(s), "Transmission signal ID: %04x", ((d[0] & 0xf) << 12) | ((d[1] & 0xf) << 8) |
                                                                         ((d[2] & 0xf) << 4) | (d[3] & 0xf));
                xds_log(c, slot, type, s);
            }
            return;
        default:
            break;
        }
    } else if (slot == 4) { /* miscellaneous */
        switch (type) {
        case 0x01:
            if (n >= 4 && (d[1] & 0x1f) < 24 && (d[0] & 0x3f) < 60 && (d[3] & 0x0f) >= 1 && (d[3] & 0x0f) <= 12) {
                int k = snprintf(s, sizeof(s), "Time of day (UTC): %02d:%02d %s %d", d[1] & 0x1f, d[0] & 0x3f,
                                 mon[d[3] & 0x0f], d[2] & 0x1f);
                if (n >= 6)
                    k += snprintf(s + k, sizeof(s) - (size_t)k, " %d, %s", 1990 + (d[5] & 0x3f), dow[d[4] & 7]);
                if (d[1] & 0x20)
                    k += snprintf(s + k, sizeof(s) - (size_t)k, ", daylight saving");
                if (d[3] & 0x10)
                    snprintf(s + k, sizeof(s) - (size_t)k, ", tape delayed");
                xds_log(c, slot, type, s); /* once a minute, as the minute changes */
            }
            return;
        case 0x04:
            if (n >= 1) {
                snprintf(s, sizeof(s), "Time zone: UTC-%d%s", d[0] & 0x1f, d[0] & 0x20 ? " (observes daylight saving)" : "");
                xds_log(c, slot, type, s);
            }
            return;
        default:
            break;
        }
    }
    else if (slot == 5 && type == 0x02) { /* weather message */
        xds_text(d, n, t, sizeof(t));
        snprintf(s, sizeof(s), "Weather: %s", t);
        xds_log(c, slot, type, s);
        return;
    }
    /* anything else: class and type, and the bytes */
    static const char *const cname[8] = { "?", "Current", "Future", "Channel", "Misc", "Public service", "Reserved", "Private" };
    int k = snprintf(s, sizeof(s), "%s type %02x:", cname[slot & 7], type);
    for (int i = 0; i < n && i < 16; i++)
        k += snprintf(s + k, sizeof(s) - (size_t)k, " %02x", d[i]);
    xds_log(c, slot, type, s);
}

static void xds_pair(cc608_t *c, int b1, int b2)
{
    if (b1 == 0x0f) { /* end of packet: b2 is the checksum */
        if (c->xcur >= 0) {
            int slot = c->xcur;
            c->xsum[slot] += 0x0f + b2;
            if ((c->xsum[slot] & 0x7f) == 0)
                xds_packet(c, slot);
            c->xcur = -1;
        }
        return;
    }
    if (b1 >= 0x01 && b1 <= 0x0e) {
        int slot = (b1 + 1) / 2;
        if (slot >= XDS_CLASSES)
            return;
        if (b1 & 1) { /* start */
            c->xlen[slot] = 0;
            c->xsum[slot] = 0;
        } else if (!c->xtype[slot] || c->xtype[slot] != b2) {
            c->xcur = -1; /* a continuation of a packet we did not see the start of */
            return;
        }
        c->xtype[slot] = b2;
        c->xsum[slot] += b1 + b2;
        c->xcur = slot;
        return;
    }
    if (c->xcur >= 0) { /* data */
        int slot = c->xcur;
        c->xsum[slot] += b1 + b2;
        if (c->xlen[slot] < XDS_MAX)
            c->xdata[slot][c->xlen[slot]++] = (uint8_t)b1;
        if (b2 && c->xlen[slot] < XDS_MAX)
            c->xdata[slot][c->xlen[slot]++] = (uint8_t)b2;
    }
}

/* ---- byte pairs ---- */

static int parity_ok(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1; /* odd parity: an odd number of ones overall */
}

void cc608_pair(cc608_t *c, int field, uint8_t r1, uint8_t r2)
{
    field &= 1;
    const int ok1 = parity_ok(r1), ok2 = parity_ok(r2);
    const int in_xds = field == 1 && (c->xcur >= 0 || (ok1 && (r1 & 0x7f) > 0 && (r1 & 0x7f) < 0x10));
    if ((!ok1 && !ok2) || ((!ok1 || !ok2) && in_xds)) {
        c->have_last[field] = 0;
        return;
    }
    int b1 = r1 & 0x7f, b2 = r2 & 0x7f;
    if (!b1 && !b2)
        return; /* padding */
    /* one byte failed its parity (47 CFR 79.101): a command is dropped, a character shows as a solid block */
    if (!ok2) {
        if (b1 >= 0x10 && b1 < 0x20) {
            c->have_last[field] = 0;
            return;
        }
        b2 = 0x7f;
    }
    if (!ok1) {
        b1 = 0x7f;
        b2 = ok2 ? b2 : 0x7f;
    }
    c->count[field]++;
    if (b1 < 0x10) {
        if (field == 1)
            xds_pair(c, b1, b2);
        return;
    }
    if (b1 < 0x20) { /* a command; broadcasters send every one twice */
        if (c->have_last[field] && c->last[field][0] == b1 && c->last[field][1] == b2) {
            c->have_last[field] = 0;
            return;
        }
        c->last[field][0] = (uint8_t)b1;
        c->last[field][1] = (uint8_t)b2;
        c->have_last[field] = 1;
        control(c, field, b1, b2);
        return;
    }
    c->have_last[field] = 0;
    if (field == 1 && c->xcur >= 0) { /* inside an XDS packet */
        xds_pair(c, b1, b2);
        return;
    }
    int idx = c->active[field];
    put(c, idx, basic(b1));
    if (b2 >= 0x20)
        put(c, idx, basic(b2));
}

/* ---- from a frame ---- */

void cc608_frame(cc608_t *c, const uint8_t *yuyv, int width, int height, int top_first)
{
    uint8_t y[MAXN];
    if (width < 400 || width > MAXN)
        return;
    const size_t stride = (size_t)width * 2;
    for (int f = 0; f < 2; f++) {
        int parity = (f == 0) == (top_first != 0) ? 0 : 1; /* the first field is the top one when top_first */
        int order[4], n = 0;
        if (c->pref[f] >= 0)
            order[n++] = c->pref[f];
        for (int r = 0; r < 4; r++)
            if (r != c->pref[f])
                order[n++] = r;
        int found = 0;
        if (c->pref[f] < 0 && c->miss[f] > 30 && (c->miss[f] & 3)) { /* nothing for a second: look every 4th frame */
            c->miss[f]++;
            continue;
        }
        for (int i = 0; i < n && !found; i++) {
            int row = order[i] * 2 + parity;
            if (row >= height)
                continue;
            const uint8_t *src = yuyv + (size_t)row * stride;
            for (int x = 0; x < width; x++)
                y[x] = src[x * 2];
            uint8_t pair[2];
            if (slice_with(&c->tset, y, width, pair)) {
                found = 1;
                c->pref[f] = order[i];
                c->miss[f] = 0;
                cc608_pair(c, f, pair[0], pair[1]);
            }
        }
        if (!found && ++c->miss[f] > 90) {
            c->pref[f] = -1;
            c->miss[f] = 0;
        }
    }
}
