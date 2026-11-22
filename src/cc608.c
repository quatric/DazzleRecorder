/* EIA-608 closed captions and XDS from the captured line 21. See cc608.h. */
#include "cc608.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- slicer ---- */

#define MAXN 768
#define MAXEDGES 256

int cc608_slice(const uint8_t *y, int n, uint8_t out[2])
{
    if (n < 400)
        return 0;
    if (n > MAXN)
        n = MAXN;
    int s[MAXN];
    s[0] = y[0];
    s[n - 1] = y[n - 1];
    for (int i = 1; i < n - 1; i++)
        s[i] = (y[i - 1] + 2 * y[i] + y[i + 1] + 2) >> 2;

    /* black and high level: the 3rd and 97th percentile of the line */
    int hist[256] = { 0 };
    for (int i = 0; i < n; i++)
        hist[s[i]]++;
    int lo = 0, hi = 255, acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc * 100 >= n * 3) {
            lo = v;
            break;
        }
    }
    acc = 0;
    for (int v = 255; v >= 0; v--) {
        acc += hist[v];
        if (acc * 100 >= n * 3) {
            hi = v;
            break;
        }
    }
    if (hi - lo < 24)
        return 0;
    const int thr = (lo + hi) / 2, hyst = (hi - lo) / 6 > 2 ? (hi - lo) / 6 : 2;

    /* threshold crossings, with hysteresis, at sub-sample positions */
    float edge[MAXEDGES];
    int rising[MAXEDGES], ne = 0;
    int state = s[0] > thr;
    for (int i = 1; i < n && ne < MAXEDGES; i++) {
        if (!state && s[i] > thr + hyst) {
            int j = i;
            while (j > 0 && s[j - 1] > thr)
                j--;
            float p = (float)j;
            if (j > 0 && s[j] != s[j - 1])
                p = (float)(j - 1) + (float)(thr - s[j - 1]) / (float)(s[j] - s[j - 1]);
            edge[ne] = p;
            rising[ne++] = 1;
            state = 1;
        } else if (state && s[i] < thr - hyst) {
            int j = i;
            while (j > 0 && s[j - 1] <= thr)
                j--;
            float p = (float)j;
            if (j > 0 && s[j] != s[j - 1])
                p = (float)(j - 1) + (float)(thr - s[j - 1]) / (float)(s[j] - s[j - 1]);
            edge[ne] = p;
            rising[ne++] = 0;
            state = 0;
        }
    }

    /* clock run-in (a sine at the bit rate: an edge every half bit), then low for the two 0 start bits, then
     * the 1 start bit's rising edge */
    for (int k = 0; k + 1 < ne; k++) {
        if (rising[k] || !rising[k + 1])
            continue;
        float gap = edge[k + 1] - edge[k];
        if (gap < 34 || gap > 78)
            continue;
        int cnt = 0, j = k;
        float sum = 0;
        while (j > 0 && edge[j] - edge[j - 1] >= 9 && edge[j] - edge[j - 1] <= 19) {
            sum += edge[j] - edge[j - 1];
            cnt++;
            j--;
        }
        if (cnt < 8)
            continue;
        float T = 2 * sum / (float)cnt;
        if (T < 25.5f || T > 28.5f)
            continue;
        if (gap < 1.6f * T || gap > 2.9f * T)
            continue;
        float ts = edge[k + 1];
        int bits[16], fits = 1;
        for (int i = 0; i < 16; i++) {
            int c = (int)(ts + ((float)i + 1.5f) * T + 0.5f);
            if (c < 1 || c + 1 >= n) {
                fits = 0;
                break;
            }
            bits[i] = (s[c - 1] + s[c] + s[c + 1]) / 3 > thr;
        }
        if (!fits)
            continue;
        int b1 = 0, b2 = 0;
        for (int i = 0; i < 8; i++) {
            b1 |= bits[i] << i;
            b2 |= bits[8 + i] << i;
        }
        out[0] = (uint8_t)b1;
        out[1] = (uint8_t)b2;
        return 1;
    }
    return 0;
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
                snprintf(s, sizeof(s), "%sProgram start %02d:%02d on %d/%d", pre, d[1] & 0x1f, d[0] & 0x3f,
                         d[3] & 0x0f, d[2] & 0x1f);
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
        case 0x05:
            if (n >= 2) {
                int c1 = d[0] & 0x3f, c2 = d[1] & 0x3f;
                static const char *const mpaa[8] = { "N/A", "G", "PG", "PG-13", "R", "NC-17", "X", "Not rated" };
                static const char *const tv[8] = { "None", "TV-Y", "TV-Y7", "TV-G", "TV-PG", "TV-14", "TV-MA", "None" };
                if ((c1 & 0x18) == 0x08) {
                    snprintf(s, sizeof(s), "%sRating: %s", pre, mpaa[c1 & 7]);
                } else if ((c1 & 0x18) == 0x10 || (c1 & 0x18) == 0x00) {
                    int k = snprintf(s, sizeof(s), "%sRating: %s", pre, tv[c2 & 7]);
                    if (c1 & 0x20 || c2 & 0x38) {
                        k += snprintf(s + k, sizeof(s) - (size_t)k, " (");
                        if (c1 & 0x20)
                            k += snprintf(s + k, sizeof(s) - (size_t)k, "D");
                        if (c2 & 0x08)
                            k += snprintf(s + k, sizeof(s) - (size_t)k, "L");
                        if (c2 & 0x10)
                            k += snprintf(s + k, sizeof(s) - (size_t)k, "S");
                        if (c2 & 0x20)
                            k += snprintf(s + k, sizeof(s) - (size_t)k, (c2 & 7) == 2 ? "FV" : "V");
                        snprintf(s + k, sizeof(s) - (size_t)k, ")");
                    }
                } else {
                    snprintf(s, sizeof(s), "%sRating (other system): %02x %02x", pre, c1, c2);
                }
                xds_log(c, slot, type, s);
            }
            return;
        default:
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
        default:
            break;
        }
    } else if (slot == 4) { /* miscellaneous */
        switch (type) {
        case 0x01:
            if (n >= 4 && (d[1] & 0x1f) < 24 && (d[0] & 0x3f) < 60) {
                snprintf(s, sizeof(s), "Time of day: %02d:%02d on %d/%d", d[1] & 0x1f, d[0] & 0x3f, d[3] & 0x0f,
                         d[2] & 0x1f);
                xds_log(c, slot, type, s);
            }
            return;
        case 0x04:
            if (n >= 1) {
                snprintf(s, sizeof(s), "Time zone: UTC-%d%s", d[0] & 0x1f, d[0] & 0x20 ? " (daylight saving)" : "");
                xds_log(c, slot, type, s);
            }
            return;
        default:
            break;
        }
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
    if (!parity_ok(r1) || !parity_ok(r2)) {
        c->have_last[field] = 0;
        return;
    }
    int b1 = r1 & 0x7f, b2 = r2 & 0x7f;
    if (!b1 && !b2)
        return; /* padding */
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
        for (int i = 0; i < n && !found; i++) {
            int row = order[i] * 2 + parity;
            if (row >= height)
                continue;
            const uint8_t *src = yuyv + (size_t)row * stride;
            for (int x = 0; x < width; x++)
                y[x] = src[x * 2];
            uint8_t pair[2];
            if (cc608_slice(y, width, pair)) {
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
