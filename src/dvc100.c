/*
 * dvc100 - userspace driver for the Dazzle DVC100 (eMPIA em28xx + SAA7113)
 *
 * Register sequences are taken from the Windows driver emDevice64.sys
 * (eMPIA "USB 28xx WDM Driver" 2.2020.1022.0). Function addresses in the
 * comments refer to that binary; see docs/REVERSE_ENGINEERING.md.
 */
#include "dvc100.h"
#include "sysaudio.h"

#include <libusb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* em28xx registers */
#define EM_R00_CHIPCFG   0x00
#define EM_R05_I2C_STAT  0x05
#define EM_R06_I2C_CLK   0x06
#define EM_R08_GPIO      0x08
#define EM_R0A_CHIPID    0x0a
#define EM_R0C_USBSUSP   0x0c
#define EM_R0E_AUDIOSRC  0x0e
#define EM_R0F_XCLK      0x0f
#define EM_R10_VINMODE   0x10
#define EM_R11_VINCTRL   0x11
#define EM_R12_VINENABLE 0x12
#define EM_R13           0x13
#define EM_R14_GAMMA     0x14
#define EM_R1B_OFLOW     0x1b
#define EM_R1C_HSTART    0x1c
#define EM_R1D_VSTART    0x1d
#define EM_R1E_CWIDTH    0x1e
#define EM_R1F_CHEIGHT   0x1f
#define EM_R20_YGAIN     0x20
#define EM_R21_YOFFSET   0x21
#define EM_R22_UVGAIN    0x22
#define EM_R25_SHARPNESS 0x25
#define EM_R26_COMPR     0x26
#define EM_R27_OUTFMT    0x27
#define EM_R28_XMIN      0x28
#define EM_R29_XMAX      0x29
#define EM_R2A_YMIN      0x2a
#define EM_R2B_YMAX      0x2b
#define EM_R30_HSCALE    0x30
#define EM_R40_AC97LSB   0x40
#define EM_R42_AC97ADDR  0x42
#define EM_R43_AC97BUSY  0x43

#define EM_CHIPCFG_VENDOR_AUDIO 0x80
#define EM_CHIPCFG_AUDIOMASK    0x30
#define EM_CHIPCFG_AC97         0x10

/* AC97 codec registers */
#define AC97_MASTER    0x02
#define AC97_MIC       0x0e
#define AC97_LINE      0x10
#define AC97_VIDEO     0x14
#define AC97_AUX       0x16
#define AC97_REC_SEL   0x1a
#define AC97_REC_GAIN  0x1c
#define AC97_POWERDOWN 0x26
#define AC97_EXT_CTRL  0x2a
#define AC97_ADC_RATE  0x32

#define VIDEO_IFACE 0
#define VIDEO_EP    0x82
#define NUM_XFERS   8
#define NUM_PKTS    64
#define AUDIO_IFACE 2
#define AUDIO_EP    0x83
#define CTRL_TIMEOUT_MS 1000

static int g_verbose;

static void dbg(int level, const char *fmt, ...)
{
    if (g_verbose < level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("dvc100: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void dvc100_set_verbose(int level) { g_verbose = level; }

static const struct { uint16_t vid, pid; const char *name; } known_devices[] = {
    { 0x2304, 0x021a, "Dazzle DVC100" },
    { 0x2304, 0x0207, "Dazzle DVC90" },
    /* em2828x family (chip id 0x9x) with the decoder built into the bridge,
     * from embda_x86_x64.inf ("Roxio/Dazzle Video Capture USB") */
    { 0x1b80, 0xe60a, "Dazzle Video Capture USB" },
    { 0x1b80, 0xe31d, "Roxio Video Capture USB" },
    { 0x1b80, 0xe31c, "Roxio Video Capture USB" },
};

typedef struct iso_stream {
    dvc100_t *owner;
    int iface;
    uint8_t ep;
    void (*packet)(dvc100_t *dev, const uint8_t *p, size_t len);
    struct libusb_transfer *xfers[8];
    uint8_t *bufs[8];
    volatile int running;
    int active;
} iso_stream_t;

static void video_packet(dvc100_t *dev, const uint8_t *p, size_t len);
static void audio_packet(dvc100_t *dev, const uint8_t *p, size_t len);

struct dvc100 {
    libusb_context *ctx;
    libusb_device_handle *h;
    dvc100_info_t info;
    dvc100_input_t input;
    dvc100_norm_t norm;
    int claimed;

    int audio_claimed;
    iso_stream_t video, audio;
    pthread_t thread;
    int thread_started;
    volatile int thread_quit;
    dvc100_assembler_t as;
    dvc100_audio_cb audio_cb;
    void *audio_user;

    /* em2828x board config from the EEPROM */
    uint16_t eeprom_base;
    uint8_t gpio[20];
    int gpio_valid;
    int alt_composite;
    uint8_t board_flags;    /* config byte +0x20 */
    uint8_t decoder_sel;    /* config byte +0x28 ("VideoDecoderI2C") */
    int reg_decoder;        /* decoder in the bridge's own register block 0x7a00 */

    sa_input_t *sys_audio;  /* audio through the OS audio input instead of libusb */
};

const char *dvc100_strerror(int err)
{
    if (err == DVC100_ERR_NO_DEVICE)
        return "no Dazzle capture device found";
    if (err == DVC100_ERR_I2C)
        return "I2C transfer failed (no ACK)";
    if (err == DVC100_ERR_TIMEOUT)
        return "timed out waiting for device";
    if (err == DVC100_ERR_NO_MEMORY)
        return "out of memory";
    return libusb_strerror((enum libusb_error)err);
}

static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

/* ---- em28xx register access (vendor request 0, emDevice64 0x140012e70) ---- */

int dvc100_reg_read(dvc100_t *dev, uint16_t reg, uint8_t *buf, int len)
{
    int r = libusb_control_transfer(dev->h, 0xc0, 0x00, 0, reg, buf, (uint16_t)len, CTRL_TIMEOUT_MS);
    if (r < 0)
        dbg(1, "reg read 0x%02x failed: %s", reg, libusb_strerror(r));
    return r < 0 ? r : 0;
}

int dvc100_reg_write(dvc100_t *dev, uint16_t reg, const uint8_t *buf, int len)
{
    int r = libusb_control_transfer(dev->h, 0x40, 0x00, 0, reg, (uint8_t *)buf, (uint16_t)len,
                                    CTRL_TIMEOUT_MS);
    if (r < 0)
        dbg(1, "reg write 0x%02x failed: %s", reg, libusb_strerror(r));
    return r < 0 ? r : 0;
}

static int rd8(dvc100_t *dev, uint16_t reg, uint8_t *v) { return dvc100_reg_read(dev, reg, v, 1); }

static int wr8(dvc100_t *dev, uint16_t reg, uint8_t v)
{
    dbg(2, "reg[0x%02x] <- 0x%02x", reg, v);
    return dvc100_reg_write(dev, reg, &v, 1);
}

static int wr16(dvc100_t *dev, uint16_t reg, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return dvc100_reg_write(dev, reg, b, 2);
}

/* read-modify-write, emDevice64 0x14001221c */
static int wr_bits(dvc100_t *dev, uint16_t reg, uint8_t val, uint8_t mask)
{
    uint8_t old;
    int r = rd8(dev, reg, &old);
    if (r)
        return r;
    return wr8(dev, reg, (uint8_t)((old & ~mask) | (val & mask)));
}

/* ---- I2C (vendor requests 2/3, emDevice64 0x1400122fc / 0x140012470) ---- */

static int i2c_wait(dvc100_t *dev)
{
    for (int i = 0; i < 40; i++) {
        uint8_t st;
        int r = rd8(dev, EM_R05_I2C_STAT, &st);
        if (r)
            return r;
        if (st == 0)
            return 0;
        if (st & 0x10)
            return DVC100_ERR_I2C;
        msleep(5);
    }
    return DVC100_ERR_TIMEOUT;
}

static int i2c_xfer_out(dvc100_t *dev, uint8_t addr, const uint8_t *buf, int len, int stop)
{
    int r = libusb_control_transfer(dev->h, 0x40, stop ? 2 : 3, 0, addr, (uint8_t *)buf,
                                    (uint16_t)len, CTRL_TIMEOUT_MS);
    if (r < 0)
        return r;
    return i2c_wait(dev);
}

int dvc100_i2c_write(dvc100_t *dev, uint8_t addr, const uint8_t *buf, int len)
{
    return i2c_xfer_out(dev, addr, buf, len, 1);
}

int dvc100_i2c_read_reg(dvc100_t *dev, uint8_t addr, uint8_t reg, uint8_t *val)
{
    int r = i2c_xfer_out(dev, addr, &reg, 1, 0);
    if (r)
        return r;
    r = libusb_control_transfer(dev->h, 0xc0, 2, 0, addr, val, 1, CTRL_TIMEOUT_MS);
    if (r < 0)
        return r;
    return i2c_wait(dev);
}

static int i2c_probe(dvc100_t *dev, uint8_t addr)
{
    uint8_t b;
    int r = libusb_control_transfer(dev->h, 0xc0, 2, 0, addr, &b, 1, CTRL_TIMEOUT_MS);
    if (r < 0)
        return 0;
    return i2c_wait(dev) == 0;
}

static int dec_wr(dvc100_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    dbg(2, "saa711x[0x%02x] <- 0x%02x", reg, val);
    int r = dvc100_i2c_write(dev, dev->info.decoder_addr, b, 2);
    if (r)
        dbg(1, "saa711x write 0x%02x failed: %s", reg, dvc100_strerror(r));
    return r;
}

/* ---- AC97 (emDevice64 0x14000d7dc / 0x14000d768) ---- */

static int ac97_wait(dvc100_t *dev)
{
    for (int i = 0; i < 100; i++) {
        uint8_t b;
        int r = rd8(dev, EM_R43_AC97BUSY, &b);
        if (r)
            return r;
        if (!(b & 1))
            return 0;
        msleep(1);
    }
    return DVC100_ERR_TIMEOUT;
}

static int ac97_write(dvc100_t *dev, uint8_t reg, uint16_t val)
{
    int r = ac97_wait(dev);
    if (r)
        return r;
    dbg(2, "ac97[0x%02x] <- 0x%04x", reg, val);
    r = wr16(dev, EM_R40_AC97LSB, val);
    if (r)
        return r;
    return wr8(dev, EM_R42_AC97ADDR, reg);
}

/* ---- device open/close ---- */

int dvc100_open(dvc100_t **out, uint16_t vid, uint16_t pid)
{
    dvc100_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return DVC100_ERR_NO_MEMORY;
    int r = libusb_init_context(&dev->ctx, NULL, 0);
    if (r) {
        free(dev);
        return r;
    }

    if (vid && pid) {
        dev->h = libusb_open_device_with_vid_pid(dev->ctx, vid, pid);
    } else {
        for (size_t i = 0; i < sizeof(known_devices) / sizeof(known_devices[0]) && !dev->h; i++) {
            dev->h = libusb_open_device_with_vid_pid(dev->ctx, known_devices[i].vid,
                                                     known_devices[i].pid);
            if (dev->h)
                dbg(1, "found %s", known_devices[i].name);
        }
    }
    if (!dev->h) {
        libusb_exit(dev->ctx);
        free(dev);
        return DVC100_ERR_NO_DEVICE;
    }

    libusb_set_auto_detach_kernel_driver(dev->h, 1);
    r = libusb_claim_interface(dev->h, VIDEO_IFACE);
    if (r) {
        dbg(0, "cannot claim video interface: %s", libusb_strerror(r));
        dvc100_close(dev);
        return r;
    }
    dev->claimed = 1;
    dev->video = (iso_stream_t){ .owner = dev, .iface = VIDEO_IFACE, .ep = VIDEO_EP, .packet = video_packet };
    dev->audio = (iso_stream_t){ .owner = dev, .iface = AUDIO_IFACE, .ep = AUDIO_EP, .packet = audio_packet };
    libusb_set_interface_alt_setting(dev->h, VIDEO_IFACE, 0);
    *out = dev;
    return 0;
}

void dvc100_close(dvc100_t *dev)
{
    if (!dev)
        return;
    dvc100_audio_stop(dev);
    dvc100_stop(dev);
    if (dev->h) {
        if (dev->audio_claimed)
            libusb_release_interface(dev->h, AUDIO_IFACE);
        if (dev->claimed) {
            libusb_set_interface_alt_setting(dev->h, VIDEO_IFACE, 0);
            libusb_release_interface(dev->h, VIDEO_IFACE);
        }
        libusb_close(dev->h);
    }
    if (dev->ctx)
        libusb_exit(dev->ctx);
    dvc100_asm_free(&dev->as);
    free(dev);
}

/* ---- SAA7113 setup (emDevice64 0x1400063b8, 0x140006478, 0x1400062c8) ---- */

/* SAA7113 analog input mode (reg 0x02 bits 0-3), registry "Mode7113" = 0x090002:
 * slot 1 tuner AI21, slot 2 composite AI11, slot 3 S-Video Y=AI12/C=AI22. */
static uint8_t saa_mode_for(dvc100_input_t in) { return in == DVC100_INPUT_SVIDEO ? 0x09 : 0x00; }

static int saa7113_init(dvc100_t *dev)
{
    /* Defaults for registers the Windows driver leaves at power-on values,
     * followed by the values it writes explicitly. */
    static const uint8_t init[][2] = {
        { 0x01, 0x08 }, { 0x03, 0x30 }, { 0x04, 0x00 }, { 0x05, 0x00 },
        { 0x06, 0xeb }, { 0x07, 0x0d }, { 0x08, 0x88 }, { 0x09, 0x01 },
        { 0x0a, 0x80 }, { 0x0b, 0x47 }, { 0x0c, 0x40 }, { 0x0d, 0x00 },
        { 0x0e, 0x01 }, { 0x0f, 0x2a }, { 0x10, 0x08 }, { 0x11, 0x0c },
        { 0x12, 0xe7 }, { 0x13, 0x00 }, { 0x15, 0x00 }, { 0x16, 0x00 },
        { 0x17, 0x00 }, { 0x40, 0x02 }, { 0x58, 0x00 }, { 0x59, 0x54 },
        { 0x5a, 0x07 }, { 0x5b, 0x83 }, { 0x5e, 0x00 },
    };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        int r = dec_wr(dev, init[i][0], init[i][1]);
        if (r)
            return r;
    }
    /* VBI slicer line control regs 0x41..0x57: all lines "active video" */
    for (uint8_t reg = 0x41; reg <= 0x57; reg++)
        dec_wr(dev, reg, 0xff);
    return 0;
}

static void em2828x_dec_input(dvc100_t *dev, int svideo);
static void regdec_input(dvc100_t *dev, int in);
static int regdec_in(dvc100_input_t input);
static int em2828x_status(dvc100_t *dev, int *locked, int *is50);
static int dec16_write(dvc100_t *dev, uint16_t reg, uint16_t mask, uint16_t val);
static int pick_alt_setting(dvc100_t *dev);

int dvc100_set_input(dvc100_t *dev, dvc100_input_t input)
{
    dev->input = input;
    if (dev->reg_decoder) {
        regdec_input(dev, regdec_in(input));
        return 0;
    }
    if (dev->info.integrated_decoder) {
        em2828x_dec_input(dev, input == DVC100_INPUT_SVIDEO);
        return 0;
    }
    int r = dec_wr(dev, 0x02, (uint8_t)(0xc0 | saa_mode_for(input)));
    if (r)
        return r;
    /* reg 0x09 bit 7 (BYPS) bypasses the chroma trap for S-Video,
     * Windows writes 0x81880deb vs 0x01880deb to regs 0x06..0x09. */
    return dec_wr(dev, 0x09, input == DVC100_INPUT_SVIDEO ? 0x81 : 0x01);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* em2828x boards use the bridge's own colour stage (regs 0x20-0x22) and the
 * decoder's hue register 0x2484 (embda64 0x14003dd08). */
int dvc100_set_brightness(dvc100_t *dev, int v)
{
    if (dev->info.integrated_decoder)
        return wr8(dev, EM_R21_YOFFSET, (uint8_t)(int8_t)clampi(v - 128, -128, 127));
    return dec_wr(dev, 0x0a, (uint8_t)clampi(v, 0, 255));
}

int dvc100_set_contrast(dvc100_t *dev, int v)
{
    if (dev->info.integrated_decoder)
        return wr8(dev, EM_R20_YGAIN, (uint8_t)clampi(v * 0x10 / 128, 0, 0x3f));
    return dec_wr(dev, 0x0b, (uint8_t)clampi(v * 0x47 / 128, 0, 127));
}

int dvc100_set_saturation(dvc100_t *dev, int v)
{
    if (dev->info.integrated_decoder)
        return wr8(dev, EM_R22_UVGAIN, (uint8_t)clampi(v * 0x10 / 128, 0, 0x3f));
    return dec_wr(dev, 0x0c, (uint8_t)clampi(v * 0x40 / 128, 0, 127));
}

int dvc100_set_hue(dvc100_t *dev, int v)
{
    if (dev->reg_decoder)
        return 0;   /* no hue control known for the 0x7a00 decoder */
    if (dev->info.integrated_decoder)
        return dec16_write(dev, 0x2484, 0xff00, (uint8_t)(int8_t)clampi(v - 128, -128, 127));
    return dec_wr(dev, 0x0d, (uint8_t)(int8_t)clampi(v - 128, -128, 127));
}

dvc100_norm_t dvc100_current_norm(dvc100_t *dev) { return dev->norm; }

static dvc100_norm_t detect_norm(dvc100_t *dev)
{
    /* emDevice64 0x140005f6c: reg 0x1f bit0 = locked, bit5 = 60 Hz */
    uint8_t st = 0;
    for (int i = 0; i < 10; i++) {
        if (dvc100_i2c_read_reg(dev, dev->info.decoder_addr, 0x1f, &st) == 0 && (st & 1))
            break;
        msleep(50);
    }
    dev->info.decoder_status = st;
    if (!(st & 1)) {
        dbg(0, "no video signal detected, defaulting to NTSC");
        return DVC100_NORM_NTSC;
    }
    return (st & 0x20) ? DVC100_NORM_NTSC : DVC100_NORM_PAL;
}

/* ---- em28xx video setup (emDevice64 0x14000c910 setup_capture_window) ---- */

static int frame_height(dvc100_norm_t n) { return n == DVC100_NORM_PAL ? 576 : 480; }

static int em28xx_video_setup(dvc100_t *dev)
{
    const int w = 720, h = frame_height(dev->norm);
    int r = 0;

    /* colour levels */
    r |= wr8(dev, EM_R14_GAMMA, 0x20);
    r |= wr8(dev, 0x15, 0x20);
    r |= wr8(dev, 0x16, 0x20);
    r |= wr8(dev, 0x17, 0x20);
    r |= wr8(dev, 0x18, 0x00);
    r |= wr8(dev, 0x19, 0x00);
    r |= wr8(dev, 0x1a, 0x00);
    r |= wr8(dev, EM_R20_YGAIN, 0x10);
    r |= wr8(dev, EM_R21_YOFFSET, 0x00);
    r |= wr8(dev, EM_R22_UVGAIN, 0x10);
    r |= wr8(dev, 0x23, 0x00);
    r |= wr8(dev, 0x24, 0x00);
    r |= wr8(dev, EM_R25_SHARPNESS, 0x00);
    r |= wr8(dev, EM_R26_COMPR, 0x00);
    r |= wr8(dev, EM_R13, 0x08);
    r |= wr_bits(dev, EM_R12_VINENABLE, 0x27, 0xaf);
    /* 0x34 = YUV422 Y0 Cb Y1 Cr, interlaced (bpp 16 branch of 0x14000d2c0) */
    r |= wr8(dev, EM_R27_OUTFMT, 0x34);
    r |= wr8(dev, EM_R10_VINMODE, 0x10);
    /* CCIR656 + interlaced input from the SAA7113 */
    r |= wr_bits(dev, EM_R11_VINCTRL, 0x11, 0x17);

    /* accumulator window */
    r |= wr8(dev, EM_R28_XMIN, 1);
    r |= wr8(dev, EM_R29_XMAX, (uint8_t)((w >> 2) - 1));
    r |= wr8(dev, EM_R2A_YMIN, 1);
    r |= wr8(dev, EM_R2B_YMAX, (uint8_t)((h >> 2) - 1));

    /* capture area */
    r |= wr8(dev, EM_R1C_HSTART, 0);
    /* vstart 2 skips the two closed-caption lines at the top of the odd field */
    r |= wr8(dev, EM_R1D_VSTART, 2);
    r |= wr8(dev, EM_R1E_CWIDTH, (uint8_t)(w >> 2));
    r |= wr8(dev, EM_R1F_CHEIGHT, (uint8_t)(h >> 2));
    r |= wr8(dev, EM_R1B_OFLOW, (uint8_t)(((w >> 2) >> 8 & 1) | (((h >> 2) >> 8 & 1) << 1)));

    /* no scaling */
    uint8_t zero[4] = { 0 };
    r |= dvc100_reg_write(dev, EM_R30_HSCALE, zero, 4);
    return r;
}

/* ---- AC97 audio routing (emDevice64 0x14000d894, 0x14000c360) ---- */

static int em28xx_audio_setup(dvc100_t *dev)
{
    if (!dev->info.has_ac97) {
        dbg(1, "no AC97 codec reported (chipcfg 0x%02x), skipping audio routing",
            dev->info.chip_cfg);
        return 0;
    }
    int r = 0;
    r |= ac97_write(dev, AC97_POWERDOWN, 0x4200);
    /* The Windows driver records the stereo mix (0x0505), but on the DVC100's
     * STAC97xx codec that path drops the left channel; record line-in directly. */
    r |= ac97_write(dev, AC97_REC_SEL, 0x0404);
    r |= ac97_write(dev, AC97_EXT_CTRL, 0x0031); /* variable rate audio */
    r |= ac97_write(dev, AC97_ADC_RATE, 48000);
    r |= ac97_write(dev, AC97_AUX, 0x8808);
    r |= ac97_write(dev, AC97_MIC, 0x8058);
    r |= ac97_write(dev, AC97_VIDEO, 0x8808);
    r |= ac97_write(dev, AC97_LINE, 0x0808);     /* DVC100 audio is wired to line-in */
    r |= ac97_write(dev, AC97_MASTER, 0x0000);
    r |= ac97_write(dev, AC97_REC_GAIN, 0x0000);
    /* audio source = line (reg 0x0e bits 6-7 = 2), then unmute (0x0f bit 7) */
    r |= wr_bits(dev, EM_R0E_AUDIOSRC, 0x80, 0xc0);
    r |= wr_bits(dev, EM_R0F_XCLK, 0x80, 0x80);
    return r;
}

/* ---- em2828x boards (embda64.sys): EEPROM GPIO map, decoder at I2C 0x82 ----
 *
 * The decoder sits on the bridge's second I2C bus (reg 0x06 bit 2) and has
 * 16-bit registers holding 16-bit values, both big endian. The board's GPIO
 * wiring (decoder power and reset) is stored in the EEPROM on the first bus. */

#include "em2828x_tables.h"

#define EM2828X_EEPROM   0xa0
#define EM2828X_DECODER  0x82

static int i2c_bus(dvc100_t *dev, int bus)
{
    /* 0x140061250: 0x40 for the EEPROM, 0x44 for everything else */
    return wr8(dev, EM_R06_I2C_CLK, bus ? 0x44 : 0x40);
}

static int i2c_read(dvc100_t *dev, uint8_t addr, uint8_t *buf, int len)
{
    int r = libusb_control_transfer(dev->h, 0xc0, 2, 0, addr, buf, (uint16_t)len, CTRL_TIMEOUT_MS);
    if (r < 0)
        return r;
    return i2c_wait(dev);
}

/* 16-bit addressed EEPROM read (0x140060e44 with a 2-byte index) */
static int eeprom_read(dvc100_t *dev, uint16_t off, uint8_t *buf, int len)
{
    int r = i2c_bus(dev, 0);
    while (!r && len > 0) {
        int n = len > 16 ? 16 : len;
        uint8_t a[2] = { (uint8_t)(off >> 8), (uint8_t)off };
        r = i2c_xfer_out(dev, EM2828X_EEPROM, a, 2, 0);
        if (!r)
            r = i2c_read(dev, EM2828X_EEPROM, buf, n);
        off += n, buf += n, len -= n;
    }
    return r;
}

static int dec16_read(dvc100_t *dev, uint16_t reg, uint16_t *val)
{
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg }, b[2];
    int r = i2c_bus(dev, 1);
    if (!r)
        r = i2c_xfer_out(dev, EM2828X_DECODER, a, 2, 0);
    if (!r)
        r = i2c_read(dev, EM2828X_DECODER, b, 2);
    if (!r)
        *val = (uint16_t)(b[0] << 8 | b[1]);
    return r;
}

/* 0x14003cacc: write the field `mask` of a register; mask 0xffff writes it whole */
static int dec16_write(dvc100_t *dev, uint16_t reg, uint16_t mask, uint16_t val)
{
    if (mask != 0xffff) {
        uint16_t old = 0;
        int sh = 0;
        while (sh < 16 && !(mask >> sh & 1))
            sh++;
        dec16_read(dev, reg, &old);
        val = (uint16_t)((old & ~mask) | ((val << sh) & mask));
    }
    dbg(2, "dec[0x%04x] <- 0x%04x", reg, val);
    uint8_t b[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(val >> 8), (uint8_t)val };
    int r = i2c_bus(dev, 1);
    if (!r)
        r = dvc100_i2c_write(dev, EM2828X_DECODER, b, 4);
    if (r)
        dbg(1, "decoder write 0x%04x failed: %s", reg, dvc100_strerror(r));
    return r;
}

/* 0x14003bbe4: apply column `col` of register table `idx` */
static void dec_table(dvc100_t *dev, int idx, int col)
{
    const uint16_t *t = dec_tables[idx].t;
    const int stride = t[dec_tables[idx].n - 1];
    if (col < 2 || col >= stride)
        return;
    uint16_t reg = 0, cur = 0, prev = 0;
    int enabled = 0, force = 0, skip = 0, skip_set = 0;
    int pos = 0;
    while (t[pos] != 5) {
        uint16_t op = t[pos];
        cur = 0;
        if (op == 0) {
            dec16_write(dev, t[pos + 1], 0xffff, t[pos + col]);
            pos += stride;
            continue;
        }
        if (op == 1) {
            reg = t[pos + 1];
            enabled = t[pos + col] != 0;
            force = 0;
            if (enabled) {
                dec16_read(dev, reg, &cur);
                prev = cur;
            }
        } else if (op == 2) {
            reg = t[pos + 1];
            enabled = force = t[pos + col] != 0;
        }
        pos += stride;
        /* op 4: field (mask, value); op 3: skip the next field if its column is 0 */
        while (t[pos] == 3 || t[pos] == 4) {
            if (enabled) {
                if (t[pos] == 3) {
                    if (t[pos + col] == 0)
                        skip = 1;
                    skip_set = 1;
                }
                if (!skip && !skip_set) {
                    uint16_t mask = t[pos + 1], v = t[pos + col];
                    if (mask) {
                        for (uint16_t m = mask; !(m & 1); m >>= 1)
                            v <<= 1;
                        v = (uint16_t)((mask & v) | (~mask & cur));
                    }
                    cur = v;
                }
                if (skip_set)
                    skip_set = 0;
                else
                    skip = 0;
            }
            pos += stride;
        }
        if (force || prev != cur)
            dec16_write(dev, reg, 0xffff, cur);
    }
}

/* EEPROM board GPIO bytes 0x4c..0x5f (0x14005f0xx). Bit 7 = used, bits 5-6 =
 * level/pulse mode, bits 0-3 = pin. GPIO function n reads byte gpio_slot[n],
 * with bit 5 inverted for the "off" twin of a function (0x14005edf4). */
static const struct { int8_t slot; uint8_t inv; } gpio_slot[0x24] = {
    [1] = { 0, 0 },  [2] = { 1, 0 },  [3] = { 2, 0 },  [4] = { 3, 0 },
    [5] = { 5, 0 },  [6] = { 5, 1 },  [7] = { 4, 0 },  [8] = { 4, 1 },
    [9] = { 6, 0 },  [10] = { 6, 1 }, [11] = { 7, 0 }, [12] = { 7, 1 },
    [13] = { 8, 0 }, [14] = { 8, 1 }, [15] = { 9, 0 }, [16] = { 9, 1 },
    [17] = { 10, 0 }, [18] = { 10, 1 }, [19] = { 11, 0 }, [20] = { 11, 1 },
    [21] = { 12, 0 }, [22] = { 12, 1 }, [23] = { 13, 0 }, [24] = { 14, 1 },
    [25] = { 14, 0 }, [26] = { 15, 0 }, [27] = { 15, 1 }, [28] = { 16, 0 },
    [29] = { 16, 1 }, [30] = { 17, 0 }, [31] = { 17, 1 }, [32] = { 18, 0 },
    [33] = { 18, 1 }, [34] = { 19, 0 }, [35] = { 19, 1 },
};

/* 0x14005f554 for em2874 and later: drive GPIO function `fn`. delay 0 sets the
 * static level, -1 the opposite one, anything else pulses for that many ms. */
static void gpio_fn(dvc100_t *dev, int fn, int delay)
{
    static const uint8_t bit[14] = { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x10, 0x20, 0x40, 2, 4, 8 };
    static const uint8_t reg[14] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x81, 0x81, 0x88, 0x88, 0x88 };
    if (!dev->gpio_valid || fn < 1 || fn >= 0x24 || (!gpio_slot[fn].slot && fn != 1))
        return;
    uint8_t b = dev->gpio[gpio_slot[fn].slot] ^ (gpio_slot[fn].inv ? 0x20 : 0);
    if (!(b & 0x80) || (b & 0x0f) >= 14)
        return;
    int mode = b & 0x60;
    if (mode == 0x40)
        mode = delay == -1 ? 0 : delay == 0 ? 0x20 : 0x40;
    else if (mode == 0x60)
        mode = delay == 0 ? 0 : delay == -1 ? 0x20 : 0x60;
    const uint8_t m = bit[b & 0x0f], r = reg[b & 0x0f];
    dbg(2, "gpio fn 0x%02x: reg 0x%02x bit 0x%02x mode 0x%02x", fn, r, m, mode);
    switch (mode) {
    case 0x00: wr_bits(dev, r, 0, m); break;
    case 0x20: wr_bits(dev, r, m, m); break;
    case 0x40: wr_bits(dev, r, 0, m); msleep(delay); wr_bits(dev, r, m, m); break;
    case 0x60: wr_bits(dev, r, m, m); msleep(delay); wr_bits(dev, r, 0, m); break;
    }
}

/* 0x14005d0b4, 0x1400599dc, 0x14005f0xx, 0x14004ac24 */
static void em2828x_read_eeprom(dvc100_t *dev)
{
    uint8_t hi = 0, sig[4] = { 0 }, flags = 0;
    uint8_t off[2] = { 0 };
    dev->eeprom_base = 0;
    dev->board_flags = 0xff;    /* driver default without an EEPROM config */
    dev->decoder_sel = 0;
    if (eeprom_read(dev, 0x0001, &hi, 1) == 0 && eeprom_read(dev, (uint16_t)(hi << 8 | 0x32), off, 2) == 0)
        dev->eeprom_base = (uint16_t)((hi << 8) + 4 + (off[0] | off[1] << 8));
    if (eeprom_read(dev, dev->eeprom_base, sig, 4) == 0 &&
        sig[0] == 0x1a && sig[1] == 0xeb && sig[2] == 0x67 && sig[3] == 0x95 &&
        eeprom_read(dev, (uint16_t)(dev->eeprom_base + 0x4c), dev->gpio, 20) == 0) {
        eeprom_read(dev, (uint16_t)(dev->eeprom_base + 0x43), &flags, 1);
        dev->alt_composite = flags >> 5 & 1;
        eeprom_read(dev, (uint16_t)(dev->eeprom_base + 0x20), &dev->board_flags, 1);
        eeprom_read(dev, (uint16_t)(dev->eeprom_base + 0x28), &dev->decoder_sel, 1);
        dbg(1, "eeprom config at 0x%04x", dev->eeprom_base);
    } else {
        /* driver defaults for boards without a configured EEPROM */
        static const uint8_t def[20] = { 0, 0xc4, 0, 0, 0, 0xa2, 0, 0x87, 0x81 };
        memcpy(dev->gpio, def, sizeof(def));
        dbg(0, "no EEPROM board config, using default GPIO map");
    }
    dev->gpio_valid = 1;
    if (g_verbose >= 1) {
        char s[80];
        int n = 0;
        for (int i = 0; i < 20; i++)
            n += snprintf(s + n, sizeof(s) - (size_t)n, "%02x ", dev->gpio[i]);
        dbg(1, "gpio map: %s", s);
    }
}

/* contrast-like curves (0x14003b5dc, 0x14003b540) */
static void dec_curve_a(dvc100_t *dev, int k)
{
    for (int u = 0; u < 0x21; u++) {
        int v = clampi((u - 16) * k * 32 / 64 + 0x200, 0, 0x3ff);
        dec16_write(dev, (uint16_t)(0x2081 + u), 0xffff, (uint16_t)v);
        dec16_write(dev, (uint16_t)(0x20a2 + u), 0xffff, (uint16_t)v);
    }
}

static void dec_curve_b(dvc100_t *dev, int offset, int gain)
{
    for (int u = 0; u < 0x21; u++) {
        int v = clampi((u * 32 - 0x1f6) * gain / 64 + 0x1f6 + offset * 8, 0, 0x3ff);
        dec16_write(dev, (uint16_t)(0x20c3 + u), 0xffff, (uint16_t)v);
    }
}

/* ADC port helpers 0x14003b690, 0x14003b884, 0x14003bb08/b924/baac, 0x14003b980 */
static void dec_port_enable(dvc100_t *dev, int port, int p3)
{
    uint16_t m1, m3, m4, m5, m6, m7;
    if (port == 1 || port == 2) {
        m1 = port == 1 ? 4 : 2, m5 = 4, m4 = 7, m3 = 0xe, m7 = 2, m6 = 1;
    } else if (port == 3 || port == 4) {
        m1 = port == 3 ? 0x10 : 8, m5 = 0x40, m4 = 0x38, m3 = 0xe0, m7 = 0x20, m6 = 0x10;
    } else {
        m1 = port == 5 ? 0x20 : 0x40, m5 = 0x400, m4 = 0x1c0, m3 = 0xe00, m7 = 0x200, m6 = 0x100;
    }
    uint16_t v = p3 ? 4 : 1;
    dec16_write(dev, 0x400, m1, 0);
    dec16_write(dev, 0x40b, m4, v);
    dec16_write(dev, 0x408, m3, v);
    dec16_write(dev, 0x408, m6, p3 == 0);
    dec16_write(dev, 0x407, m7, p3 == 0);
    dec16_write(dev, 0x407, m5, 1);
    if (port == 2)
        dec16_write(dev, 0x407, 1, 0);
    if (port == 4)
        dec16_write(dev, 0x407, 0x10, 0);
}

static void dec_port_gain(dvc100_t *dev, int port, uint16_t v)
{
    static const uint16_t reg[6] = { 0, 0x402, 0x401, 0x404, 0x403, 0x405 };
    dec16_write(dev, reg[port], 0x1f, v);
}

static uint16_t port_sel(int port)
{
    static const uint16_t v[6] = { 0, 1, 0, 3, 2, 4 };
    return v[port];
}

static void dec_port_clamp(dvc100_t *dev, int port, uint16_t v)
{
    static const uint16_t reg[6] = { 0, 0x40d, 0x40c, 0x40f, 0x40e, 0x410 };
    static const uint16_t bit[6] = { 0, 0x10, 1, 0x1000, 0x100, 1 };
    dec16_write(dev, 0x230b, 0x3f, 0x12);
    dec16_write(dev, 0x230b, 0xfc00, 0x20);
    dec16_write(dev, reg[port], 4, v >> 1 & 1);
    dec16_write(dev, reg[port], 2, v & 1);
    dec16_write(dev, port == 5 ? 0x2602 : 0x2601, bit[port], 1);
}

/* 0x14003b3e4 */
static void em2828x_dec_reset(dvc100_t *dev)
{
    dec_table(dev, 0x0e, 4);
    dec_table(dev, 0x0f, 2);
    dec16_write(dev, 0x100, 0x40, 0);
    msleep(100);
}

/* 0x14003c6b4 + 0x14003c8a0: power up the decoder blocks and set the standard */
static void em2828x_dec_standard(dvc100_t *dev, int svideo, int pal)
{
    dec16_write(dev, 0x100, 0x700f, 0);
    dec16_write(dev, 0x003, 1, 1);
    dec16_write(dev, 0x000, 1, 0);
    dec16_write(dev, 0x2306, 0x7f00, 8);
    dec16_write(dev, 0x2100, 0x2000, 0);
    dec16_write(dev, 0x2106, 0xc000, 1);
    dec_curve_a(dev, 0x44);
    dec_curve_b(dev, 4, 0x3f);
    dec16_write(dev, 0x2002, 0xffff, 6);
    dec16_write(dev, 0x2003, 0xffff, 3);
    dec16_write(dev, 0x2080, 0xffff, 0);
    dec16_write(dev, 0x400, 0x181, 0);
    dec16_write(dev, 0x400, 0x200, 1);
    dec_table(dev, 0x0b, 4);
    em2828x_dec_reset(dev);

    /* table columns for NTSC-M (standard 1) and PAL-B/G/I (0x10) */
    int c12 = pal ? 3 : 2, c0 = pal ? 4 : 7, c2 = pal ? 2 : 4, c3 = pal ? 2 : 3;
    dec_table(dev, 0x12, c12);
    dec16_write(dev, 0x2484, 0x7f, pal ? 2 : 0x20);
    dec_table(dev, 0x00, c0);
    dec_table(dev, 0x02, c2);
    if (!svideo)
        dec_table(dev, 0x03, c3);
    dec_table(dev, 0x05, svideo ? 5 : 3);
    /* 0x14003bb64 */
    dec16_write(dev, 0x210b, 0xffff, 0x40);
    dec16_write(dev, 0x210c, 0xffff, 0);
    dec16_write(dev, 0x2106, 3, 2);
    dec16_write(dev, 0x2106, 4, 0);
    dec16_write(dev, 0x2106, 0x2000, 1);
}

/* 0x14003e110 + 0x14003c010: route composite (Windows input 1) or S-Video (2) */
static void em2828x_dec_input(dvc100_t *dev, int svideo)
{
    /* no tuner: reg 0x11 bit 5 on, reg 0xb6 bit 7 off (0x14005c734) */
    wr_bits(dev, EM_R11_VINCTRL, 0x20, 0x20);
    wr_bits(dev, 0xb6, 0, 0x80);
    gpio_fn(dev, 0x20, 0);

    if (!svideo) {
        int port = dev->alt_composite ? 3 : 2;
        dec_table(dev, 0x06, 4);
        dec16_write(dev, 0x101, 0xffff, 0);
        dec16_write(dev, 0x400, 0x3e, 0x3e);
        dec_port_enable(dev, port, 0);
        dec_port_gain(dev, port, 1);
        dec16_write(dev, 0x2600, 7, port_sel(port));
        dec_port_clamp(dev, port, 0);
    } else {
        int luma = dev->alt_composite ? 5 : 3;
        dec_table(dev, 0x06, 4);
        dec_table(dev, 0x05, 5);
        dec16_write(dev, 0x101, 0xffff, 0);
        dec16_write(dev, 0x400, 0x3e, 0x3e);
        dec_port_enable(dev, luma, 0);
        dec_port_enable(dev, 4, 0);
        dec_port_gain(dev, luma, 2);
        dec16_write(dev, 0x2600, 7, port_sel(luma));
        dec_port_clamp(dev, luma, 0);
        dec_port_gain(dev, 4, 2);
        dec16_write(dev, 0x2600, 0x38, port_sel(4));
        dec_port_clamp(dev, 4, 2);
    }
    dec16_write(dev, 0x2585, 0x300, 2);
    dec16_write(dev, 0x2302, 0xc, 0);
    /* 0x14003c820 */
    dec16_write(dev, 0x2300, 0x1c, 7);
    dec16_write(dev, 0x2300, 3, 3);
    dec16_write(dev, 0x2300, 0x600, 1);
}

/* ---- decoder in the bridge's register block 0x7a00 (emDevice64 decoder type 0x104a,
 * picked when EEPROM config byte +0x28 is 0x10). The DVC100 driver drives the black
 * Dazzle this way; embda64 instead looks for an I2C decoder that this board lacks. */

/* 0x1400085d4 */
static void regdec_base(dvc100_t *dev)
{
    wr8(dev, EM_R1C_HSTART, 2);
    wr8(dev, EM_R1D_VSTART, 0);
    wr8(dev, 0x7a0b, 0x00);
    wr8(dev, 0x7a1c, 0x1e);
    wr8(dev, 0x7a1d, 0x99);
    wr8(dev, 0x7a1e, 0x99);
    wr8(dev, 0x7a1f, 0x9a);
    wr8(dev, 0x7a2f, 0x52);
    wr8(dev, 0x7a40, 0x05);
    wr8(dev, 0x7a3f, 1);
    wr8(dev, 0x7a3f, 0);
}

/* 0x140008690 for NTSC-M (Windows standard 0x1) and PAL-B/G/D (0x10). `in` is the
 * Windows input: 2 composite, 3 S-Video. */
static void regdec_standard(dvc100_t *dev, int pal, int in)
{
    const int svideo = in == 3;
    wr8(dev, 0x24, 0);
    wr8(dev, 0x7a02, in == 2 ? 0x4f : 0x4e);
    wr8(dev, 0x7a20, 0x3d);
    wr8(dev, 0x7a21, 0x3e);
    wr8(dev, 0x7a51, 0);
    wr8(dev, 0x7ac1, 0x1b);
    if (!pal) {
        static const uint16_t base[][2] = {
            { 0x7a00, 0x00 }, { 0x7a01, 0x05 }, { 0x7a04, 0xdd }, { 0x7a0c, 0x8a },
            { 0x7a0f, 0x1c }, { 0x7a18, 0x20 }, { 0x7a19, 0x74 }, { 0x7a1a, 0x5d },
            { 0x7a1b, 0x17 }, { 0x7a2e, 0x85 }, { 0x7a82, 0x42 },
        };
        for (size_t i = 0; i < sizeof(base) / sizeof(*base); i++)
            wr8(dev, base[i][0], (uint8_t)base[i][1]);
        wr8(dev, 0x7a00, svideo ? 0x01 : 0x00);
        wr8(dev, 0x7a01, 0x05);
        wr8(dev, 0x7a03, svideo ? 0x03 : 0x00);
        wr8(dev, 0x7a07, 0x60);
        wr8(dev, 0x7a08, 0x70);
        wr8(dev, 0x7a09, 0x10);
        wr8(dev, 0x7a0a, 0x7c);
        wr8(dev, 0x7a2e, 0x85);
        wr8(dev, 0x7a30, svideo ? 0x20 : 0x22);
        wr8(dev, 0x7a80, svideo ? 4 : 3);
        wr8(dev, 0x7ac0, 0xd4);
        wr8(dev, 0x7a31, 0x63);
        wr8(dev, 0x7a50, 0xdd);
        wr8(dev, 0x7a5d, 0x0e);
        wr8(dev, 0x7a5e, 0xea);
        wr8(dev, 0x7a60, 0x64);
        wr8(dev, 0x7a67, 0x5a);
        wr8(dev, 0xb0, 0x38);
        wr8(dev, 0xb2, 0x38);
    } else {
        static const uint16_t base[][2] = {
            { 0x7a01, 0x04 }, { 0x7a04, 0xdc }, { 0x7a0c, 0x67 }, { 0x7a0f, 0x1c },
            { 0x7a18, 0x28 }, { 0x7a19, 0x32 }, { 0x7a1a, 0xb9 }, { 0x7a1b, 0x86 },
            { 0x7a82, 0x52 },
        };
        for (size_t i = 0; i < sizeof(base) / sizeof(*base); i++)
            wr8(dev, base[i][0], (uint8_t)base[i][1]);
        if (!svideo) {
            wr8(dev, 0x7a00, 0x32);
            wr8(dev, 0x7a01, 0x10);
            wr8(dev, 0x7a03, 0x06);
            wr8(dev, 0x7a07, 0x2f);
            wr8(dev, 0x7a08, 0x77);
            wr8(dev, 0x7a09, 0x0f);
            wr8(dev, 0x7a0a, 0x8c);
            wr8(dev, 0x7a20, 0x3d);
            wr8(dev, 0x7a2e, 0x88);
            wr8(dev, 0x7a30, 0x2a);
            wr8(dev, 0x7a80, 7);
        } else {
            wr8(dev, 0x7a00, 0x33);
            wr8(dev, 0x7a01, 0x04);
            wr8(dev, 0x7a03, 0x04);
            wr8(dev, 0x7a07, 0x20);
            wr8(dev, 0x7a08, 0x6a);
            wr8(dev, 0x7a09, 0x16);
            wr8(dev, 0x7a0a, 0x80);
            wr8(dev, 0x7a2e, 0x8a);
            wr8(dev, 0x7a30, 0x26);
            wr8(dev, 0x7a80, 8);
        }
        wr8(dev, 0x7a28, 0x5e);
        wr8(dev, 0x7a31, 0xc3);
        wr8(dev, 0x7a53, 0xcc);
        wr8(dev, 0x7a5d, 0x16);
        wr8(dev, 0x7a5e, 0x50);
        wr8(dev, 0x7a60, 0xb4);
        wr8(dev, 0x7a67, 100);
        wr8(dev, 0xb0, svideo ? 0x38 : 0x3a);
        wr8(dev, 0xb2, svideo ? 0x3a : 0x38);
    }
    wr8(dev, 0x7a3f, 1);
    wr8(dev, 0x7a3f, 0);
}

/* 0x140008508: analog mux, then the standard again for the new input */
static void regdec_input(dvc100_t *dev, int in)
{
    if (in != 1)
        wr8(dev, 0x7a29, 0);
    wr8(dev, 0x38, in == 3 ? 0 : 1);
    wr8(dev, 0x2e, 0);
    wr8(dev, 0xb8, in == 1 ? 1 : 0);
    wr8(dev, 0xb6, 0x8f);
    regdec_standard(dev, dev->norm == DVC100_NORM_PAL, in);
}

/* Windows input 1 is a tuner/unused routing on this board (blue no-signal screen);
 * the composite jack is input 2. */
static int regdec_in(dvc100_input_t input) { return input == DVC100_INPUT_SVIDEO ? 3 : 2; }

/* 0x14000833c reads 0x7a69 bits 6-7 (525-line) or 0x7a6a bits 4-5, but on the black
 * Dazzle those stay 0 with a picture present. Measured on hardware: 0x7a6c bit 7 and
 * 0x7a6e bit 7 are set only while a signal is present. */
static int regdec_locked(dvc100_t *dev, int pal)
{
    uint8_t st = 0, v = 0, a = 0, b = 0;
    rd8(dev, 0x7a65, &st);
    rd8(dev, pal ? 0x7a6a : 0x7a69, &v);
    rd8(dev, 0x7a6c, &a);
    rd8(dev, 0x7a6e, &b);
    wr8(dev, 0x7a65, st & 0x20);
    int win = pal ? (v >> 4 & 3) != 0 : v >> 6 == 3;
    return win || ((a & 0x80) && (b & 0x80));
}

static int regdec_init(dvc100_t *dev, dvc100_input_t input, dvc100_norm_t norm)
{
    int r;
    dev->reg_decoder = 1;
    dev->input = input;
    dev->norm = norm == DVC100_NORM_PAL ? DVC100_NORM_PAL : DVC100_NORM_NTSC;
    if ((r = em28xx_video_setup(dev)))
        return r;
    regdec_base(dev);
    regdec_input(dev, regdec_in(input));
    if (norm == DVC100_NORM_AUTO) {
        int locked = 0;
        for (int i = 0; i < 10 && !(locked = regdec_locked(dev, 0)); i++)
            msleep(50);
        if (!locked) {
            dev->norm = DVC100_NORM_PAL;
            regdec_input(dev, regdec_in(input));
            for (int i = 0; i < 10 && !(locked = regdec_locked(dev, 1)); i++)
                msleep(50);
            if (!locked) {
                dbg(0, "no video signal detected, defaulting to NTSC");
                dev->norm = DVC100_NORM_NTSC;
                regdec_input(dev, regdec_in(input));
            }
        }
        if (dev->norm == DVC100_NORM_PAL) {
            /* frame height changes with the standard */
            if ((r = em28xx_video_setup(dev)))
                return r;
            regdec_base(dev);
            regdec_input(dev, regdec_in(input));
        }
    } else {
        /* give the decoder time to lock so the first status read is meaningful */
        for (int i = 0; i < 10 && !regdec_locked(dev, dev->norm == DVC100_NORM_PAL); i++)
            msleep(50);
    }
    dbg(1, "built-in decoder (0x7a00), video standard: %s",
        dev->norm == DVC100_NORM_PAL ? "PAL" : "NTSC");
    return pick_alt_setting(dev);
}

/* 0x14003bfec / 0x14003bf10: reg 0x2282 bits 8-9 = sync lock, 0x2312 bit 2 = 50 Hz */
static int em2828x_status(dvc100_t *dev, int *locked, int *is50)
{
    if (dev->reg_decoder) {
        *is50 = dev->norm == DVC100_NORM_PAL;
        *locked = regdec_locked(dev, *is50);
        return 0;
    }
    uint16_t a = 0, b = 0;
    int r = dec16_read(dev, 0x2282, &a);
    if (!r)
        r = dec16_read(dev, 0x2312, &b);
    *locked = (a >> 8 & 3) == 3;
    *is50 = !!(b & 4);
    return r;
}

static int em2828x_init(dvc100_t *dev, dvc100_input_t input, dvc100_norm_t norm)
{
    int r;
    dev->info.integrated_decoder = 1;
    /* device start (embda64 0x14004c014): GPIO port 0 to the "GPIODefault" 0xff
     * and reg 0x0d = 0xff */
    wr8(dev, 0x80, 0xff);
    wr8(dev, 0x0d, 0xff);
    wr8(dev, EM_R06_I2C_CLK, 0x40);
    em2828x_read_eeprom(dev);
    /* 0x14004c988: unless config byte +0x20 bit 1 is set, clear regs 0xb1/0xb3/0xb5
     * and set 0xb6 = 0x0f before anything talks to the built-in decoder */
    if (!(dev->board_flags & 2)) {
        wr8(dev, 0xb1, 0);
        wr8(dev, 0xb3, 0);
        wr8(dev, 0xb5, 0);
        wr8(dev, 0xb6, 0x0f);
    }
    dbg(1, "eeprom decoder select 0x%02x", dev->decoder_sel);
    if (dev->decoder_sel == 0x10)
        return regdec_init(dev, input, norm);

    /* decoder power and reset (0x14005b104) */
    gpio_fn(dev, 0x0d, 0);
    gpio_fn(dev, 0x08, 0);
    msleep(100);
    gpio_fn(dev, 0x17, 50);
    msleep(50);

    uint16_t id = 0;
    if ((r = dec16_read(dev, 0x0000, &id))) {
        dbg(0, "decoder at I2C 0x%02x does not answer: %s", EM2828X_DECODER, dvc100_strerror(r));
        i2c_bus(dev, 0);
        return r;
    }
    dev->info.decoder_addr = EM2828X_DECODER;
    dev->info.decoder_ver = (uint8_t)id;
    dbg(1, "decoder at 0x%02x, reg 0x0000 = 0x%04x", EM2828X_DECODER, id);
    em2828x_dec_reset(dev);

    dev->input = input;
    int svideo = input == DVC100_INPUT_SVIDEO;
    em2828x_dec_standard(dev, svideo, norm == DVC100_NORM_PAL);
    em2828x_dec_input(dev, svideo);
    if (norm == DVC100_NORM_AUTO) {
        int locked = 0, is50 = 0;
        for (int i = 0; i < 10 && !locked; i++) {
            msleep(50);
            em2828x_status(dev, &locked, &is50);
        }
        norm = locked && is50 ? DVC100_NORM_PAL : DVC100_NORM_NTSC;
        if (!locked)
            dbg(0, "no video signal detected, defaulting to NTSC");
        else if (is50)
            em2828x_dec_standard(dev, svideo, 1);
    }
    dev->norm = norm;
    dbg(1, "video standard: %s", dev->norm == DVC100_NORM_PAL ? "PAL" : "NTSC");

    if ((r = em28xx_video_setup(dev)))
        return r;
    return pick_alt_setting(dev);
}

static int pick_alt_setting(dvc100_t *dev)
{
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(libusb_get_device(dev->h), &cfg);
    if (r)
        return r;
    int best_alt = -1, best = 0;
    const struct libusb_interface *itf = &cfg->interface[VIDEO_IFACE];
    for (int a = 0; a < itf->num_altsetting; a++) {
        const struct libusb_interface_descriptor *d = &itf->altsetting[a];
        for (int e = 0; e < d->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
            if (ep->bEndpointAddress != VIDEO_EP ||
                (ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
                continue;
            int mps = ep->wMaxPacketSize;
            int size = (mps & 0x7ff) * (1 + ((mps >> 11) & 3));
            dbg(1, "alt %d: ep 0x82 packet size %d", d->bAlternateSetting, size);
            if (size > best) {
                best = size;
                best_alt = d->bAlternateSetting;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (best_alt < 0)
        return LIBUSB_ERROR_NOT_FOUND;
    dev->info.alt_setting = best_alt;
    dev->info.iso_packet_size = best;
    return 0;
}

int dvc100_init(dvc100_t *dev, dvc100_input_t input, dvc100_norm_t norm)
{
    int r;
    if ((r = rd8(dev, EM_R0A_CHIPID, &dev->info.chip_id)))
        return r;
    if ((r = rd8(dev, EM_R00_CHIPCFG, &dev->info.chip_cfg)))
        return r;
    dev->info.has_ac97 = (dev->info.chip_cfg & EM_CHIPCFG_AUDIOMASK) == EM_CHIPCFG_AC97;
    dev->info.vendor_audio = !!(dev->info.chip_cfg & EM_CHIPCFG_VENDOR_AUDIO);
    dbg(1, "chip id 0x%02x, chipcfg 0x%02x", dev->info.chip_id, dev->info.chip_cfg);

    /* em2828x (chip class 6 in embda64.sys 0x14005c3e4) */
    if ((dev->info.chip_id & 0xf0) == 0x90)
        return em2828x_init(dev, input, norm);

    /* I2C: wait enable, 100 kHz (0x14000d5c0) */
    wr8(dev, EM_R06_I2C_CLK, 0x40);
    /* GPIO bit 3 powers the decoder (0x14000e1bc), then 100 ms settle */
    wr_bits(dev, EM_R08_GPIO, 0x08, 0x08);
    msleep(100);

    /* Decoder detection order from 0x140005488 */
    static const uint8_t probe[] = { 0x4a, 0x48, 0x42 };
    for (size_t i = 0; i < sizeof(probe); i++) {
        if (i2c_probe(dev, probe[i])) {
            dev->info.decoder_addr = probe[i];
            break;
        }
    }
    if (!dev->info.decoder_addr) {
        dbg(0, "no SAA711x video decoder answered on I2C");
        return DVC100_ERR_I2C;
    }
    dvc100_i2c_read_reg(dev, dev->info.decoder_addr, 0x00, &dev->info.decoder_ver);
    dbg(1, "decoder at 0x%02x, version 0x%02x", dev->info.decoder_addr, dev->info.decoder_ver);

    if ((r = saa7113_init(dev)))
        return r;
    if ((r = dvc100_set_input(dev, input)))
        return r;

    dev->norm = norm == DVC100_NORM_AUTO ? detect_norm(dev) : norm;
    dbg(1, "video standard: %s", dev->norm == DVC100_NORM_PAL ? "PAL" : "NTSC");

    if ((r = em28xx_video_setup(dev)))
        return r;
    em28xx_audio_setup(dev);
    return pick_alt_setting(dev);
}

int dvc100_get_info(dvc100_t *dev, dvc100_info_t *info)
{
    if (dev->info.integrated_decoder) {
        int locked = 0, is50 = 0;
        /* reported in the SAA7113 layout: bit 0 locked, bit 5 60 Hz */
        if (em2828x_status(dev, &locked, &is50) == 0)
            dev->info.decoder_status = (uint8_t)((locked ? 1 : 0) | (is50 ? 0 : 0x20));
    } else if (dev->info.decoder_addr) {
        dvc100_i2c_read_reg(dev, dev->info.decoder_addr, 0x1f, &dev->info.decoder_status);
    }
    *info = dev->info;
    return 0;
}

/* ---- field assembler (emDevice64 0x14000acdc) ---- */

int dvc100_asm_init(dvc100_assembler_t *a, int width, int height, dvc100_frame_cb cb, void *user)
{
    memset(a, 0, sizeof(*a));
    a->width = width;
    a->height = height;
    a->frame = calloc((size_t)width * height, 2);
    if (!a->frame)
        return DVC100_ERR_NO_MEMORY;
    /* black in YUYV */
    for (size_t i = 0; i < (size_t)width * height * 2; i += 2) {
        a->frame[i] = 0x10;
        a->frame[i + 1] = 0x80;
    }
    a->cb = cb;
    a->user = user;
    return 0;
}

void dvc100_asm_free(dvc100_assembler_t *a)
{
    free(a->frame);
    a->frame = NULL;
}

static void asm_emit(dvc100_assembler_t *a)
{
    if (a->got_top && a->got_bottom) {
        dvc100_frame_t f = { a->frame, a->width, a->height, a->sequence++, a->first_top };
        if (a->cb)
            a->cb(&f, a->user);
    } else if (a->got_top || a->got_bottom) {
        a->dropped++;
    }
    a->got_top = a->got_bottom = 0;
}

void dvc100_asm_packet(dvc100_assembler_t *a, const uint8_t *p, size_t len)
{
    if (len >= 4 && p[0] == 0x88 && p[1] == 0x88 && p[2] == 0x88 && p[3] == 0x88) {
        p += 4;
        len -= 4;
    }
    if (len >= 4 && p[0] == 0x33 && p[1] == 0x95) {
        /* VBI block, VBI capture is never enabled */
        a->have_field = 0;
        return;
    }
    if (len >= 4 && p[0] == 0x22 && p[1] == 0x5a) {
        /* start of a field, bit 0 of byte 2 set = odd (bottom) field */
        int top = !(p[2] & 1);
        if (top || (!a->got_top && !a->got_bottom))
            asm_emit(a);
        if (!a->got_top && !a->got_bottom)
            a->first_top = top;
        a->cur_top = top;
        a->field_off = 0;
        a->have_field = 1;
        if (top)
            a->got_top = 1;
        else
            a->got_bottom = 1;
        p += 4;
        len -= 4;
    }
    if (!a->have_field || !len)
        return;

    const size_t stride = (size_t)a->width * 2;
    const size_t field_bytes = stride * (size_t)(a->height / 2);
    while (len && a->field_off < field_bytes) {
        size_t line = a->field_off / stride, col = a->field_off % stride;
        size_t n = stride - col;
        if (n > len)
            n = len;
        size_t row = line * 2 + (a->cur_top ? 0 : 1);
        memcpy(a->frame + row * stride + col, p, n);
        a->field_off += n;
        p += n;
        len -= n;
    }
    if (a->field_off >= field_bytes && !a->cur_top) {
        /* bottom field complete: emit now instead of waiting for the next header */
        asm_emit(a);
        a->have_field = 0;
    }
}

/* ---- isochronous streaming ---- */

/* One libusb event thread serves both streams; it runs while either is active. */
static void *event_thread(void *arg)
{
    dvc100_t *dev = arg;
    while (!dev->thread_quit) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    }
    return NULL;
}

static void LIBUSB_CALL iso_cb(struct libusb_transfer *xfer)
{
    iso_stream_t *s = xfer->user_data;
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < xfer->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *pd = &xfer->iso_packet_desc[i];
            if (pd->status != LIBUSB_TRANSFER_COMPLETED || !pd->actual_length)
                continue;
            s->packet(s->owner, libusb_get_iso_packet_buffer_simple(xfer, (unsigned)i),
                      pd->actual_length);
        }
    } else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        dbg(1, "iso transfer on ep 0x%02x: status %d", s->ep, xfer->status);
    }
    if (s->running && xfer->status != LIBUSB_TRANSFER_NO_DEVICE &&
        libusb_submit_transfer(xfer) == 0)
        return;
    __atomic_sub_fetch(&s->active, 1, __ATOMIC_SEQ_CST);
}

static void stream_stop(dvc100_t *dev, iso_stream_t *s)
{
    if (!s->running && !s->active)
        return;
    s->running = 0;
    for (int i = 0; i < NUM_XFERS; i++)
        if (s->xfers[i])
            libusb_cancel_transfer(s->xfers[i]);
    /* the event thread reaps the cancelled transfers */
    for (int t = 0; t < 2000 && __atomic_load_n(&s->active, __ATOMIC_SEQ_CST) > 0; t++)
        msleep(1);
    for (int i = 0; i < NUM_XFERS; i++) {
        if (s->xfers[i])
            libusb_free_transfer(s->xfers[i]);
        free(s->bufs[i]);
        s->xfers[i] = NULL;
        s->bufs[i] = NULL;
    }
    s->active = 0;
    libusb_set_interface_alt_setting(dev->h, s->iface, 0);
    if (!dev->video.running && !dev->audio.running && dev->thread_started) {
        dev->thread_quit = 1;
        pthread_join(dev->thread, NULL);
        dev->thread_started = 0;
    }
}

static int stream_start(dvc100_t *dev, iso_stream_t *s, int alt, int psize, int npkts)
{
    int r = libusb_set_interface_alt_setting(dev->h, s->iface, alt);
    if (r)
        return r;
    if (!dev->thread_started) {
        dev->thread_quit = 0;
        if (pthread_create(&dev->thread, NULL, event_thread, dev))
            return DVC100_ERR_NO_MEMORY;
        dev->thread_started = 1;
    }
    s->running = 1;
    for (int i = 0; i < NUM_XFERS; i++) {
        s->xfers[i] = libusb_alloc_transfer(npkts);
        s->bufs[i] = malloc((size_t)psize * npkts);
        if (!s->xfers[i] || !s->bufs[i]) {
            r = DVC100_ERR_NO_MEMORY;
            break;
        }
        libusb_fill_iso_transfer(s->xfers[i], dev->h, s->ep, s->bufs[i], psize * npkts, npkts,
                                 iso_cb, s, 0);
        libusb_set_iso_packet_lengths(s->xfers[i], (unsigned)psize);
        if ((r = libusb_submit_transfer(s->xfers[i]))) {
            dbg(0, "submit iso transfer on ep 0x%02x: %s", s->ep, libusb_strerror(r));
            break;
        }
        __atomic_add_fetch(&s->active, 1, __ATOMIC_SEQ_CST);
    }
    if (r)
        stream_stop(dev, s);
    return r;
}

static void video_packet(dvc100_t *dev, const uint8_t *p, size_t len)
{
    if (g_verbose >= 3) {
        /* packet trace: sizes and the first bytes of packets that look like headers */
        static uint64_t n, bytes, hdr;
        static int shown;
        n++;
        bytes += len;
        if (len >= 4 && (p[0] == 0x22 || p[0] == 0x33 || p[0] == 0x88)) {
            hdr++;
            if (shown < 12) {
                shown++;
                dbg(3, "pkt %llu len %zu: %02x %02x %02x %02x %02x %02x %02x %02x", (unsigned long long)n,
                    len, p[0], p[1], p[2], p[3], len > 4 ? p[4] : 0, len > 5 ? p[5] : 0,
                    len > 6 ? p[6] : 0, len > 7 ? p[7] : 0);
            }
        }
        if (n % 8000 == 0)
            dbg(3, "%llu packets, %llu bytes, %llu header-like", (unsigned long long)n,
                (unsigned long long)bytes, (unsigned long long)hdr);
    }
    dvc100_asm_packet(&dev->as, p, len);
}

int dvc100_start(dvc100_t *dev, dvc100_frame_cb cb, void *user)
{
    int r;
    if (dev->video.running)
        return 0;
    dvc100_asm_free(&dev->as);
    if ((r = dvc100_asm_init(&dev->as, 720, frame_height(dev->norm), cb, user)))
        return r;
    dbg(1, "streaming on alt %d, %d bytes/packet", dev->info.alt_setting,
        dev->info.iso_packet_size);

    /* start capture (0x14000e26c) */
    wr_bits(dev, EM_R0C_USBSUSP, 0x10, 0x10);
    wr_bits(dev, EM_R12_VINENABLE, 0x40, 0x40);
    msleep(10);
    return stream_start(dev, &dev->video, dev->info.alt_setting, dev->info.iso_packet_size,
                        NUM_PKTS);
}

void dvc100_stop(dvc100_t *dev)
{
    if (!dev->video.running && !dev->video.active)
        return;
    stream_stop(dev, &dev->video);
    /* stop capture (0x14000e234) */
    wr_bits(dev, EM_R12_VINENABLE, 0x00, 0x40);
    msleep(33);
    wr_bits(dev, EM_R0C_USBSUSP, 0x00, 0x10);
}

/* ---- audio: USB Audio Class 1 streaming interface, read directly ----
 * On macOS AppleUSBAudio matches the DVC100's AudioControl interface but
 * never publishes a device for it (UAC1 on a high-speed bus, one packet per
 * microframe), so the streaming interface is free to claim. */

static void audio_packet(dvc100_t *dev, const uint8_t *p, size_t len)
{
    if (dev->audio_cb)
        dev->audio_cb((const int16_t *)(const void *)p, (int)(len / 4), dev->audio_user);
}

static int pick_audio_alt(dvc100_t *dev, int *alt, int *psize, int *rate)
{
    struct libusb_config_descriptor *cfg;
    int r = libusb_get_active_config_descriptor(libusb_get_device(dev->h), &cfg);
    if (r)
        return r;
    *alt = -1;
    if (cfg->bNumInterfaces > AUDIO_IFACE) {
        const struct libusb_interface *itf = &cfg->interface[AUDIO_IFACE];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *d = &itf->altsetting[a];
            if (d->bNumEndpoints < 1 || d->endpoint[0].bEndpointAddress != AUDIO_EP)
                continue;
            /* class-specific FORMAT_TYPE: 0x24 0x02, one discrete rate at offset 8 */
            int fr = 0, channels = 0, bits = 0;
            for (int o = 0; o + 2 < d->extra_length; o += d->extra[o]) {
                const uint8_t *x = d->extra + o;
                if (!x[0])
                    break;
                if (x[1] == 0x24 && x[2] == 0x02 && x[0] >= 11) {
                    channels = x[4];
                    bits = x[6];
                    fr = x[8] | x[9] << 8 | x[10] << 16;
                }
            }
            dbg(1, "audio alt %d: %d Hz, %d ch, %d bit, packet %d", d->bAlternateSetting, fr,
                channels, bits, d->endpoint[0].wMaxPacketSize);
            if (channels == 2 && bits == 16 && (fr == 48000 || *alt < 0)) {
                *alt = d->bAlternateSetting;
                *psize = d->endpoint[0].wMaxPacketSize & 0x7ff;
                *rate = fr;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return *alt < 0 ? LIBUSB_ERROR_NOT_FOUND : 0;
}

/* The OS audio input device for this box (Core Audio, WASAPI or ALSA). */
static int start_system_audio(dvc100_t *dev, dvc100_audio_cb cb, void *user, int *rate)
{
    struct libusb_device_descriptor dd;
    char product[128] = "";
    libusb_get_device_descriptor(libusb_get_device(dev->h), &dd);
    if (dd.iProduct)
        libusb_get_string_descriptor_ascii(dev->h, dd.iProduct, (unsigned char *)product, sizeof(product));
    dev->sys_audio = sa_input_start(dd.idVendor, dd.idProduct, product, cb, user, rate);
    if (!dev->sys_audio)
        return LIBUSB_ERROR_NOT_FOUND;
    dbg(1, "audio through the system input device (%s) at %d Hz", sa_backend_name(), rate ? *rate : 0);
    return 0;
}

int dvc100_audio_start(dvc100_t *dev, dvc100_audio_cb cb, void *user, int *rate)
{
    int alt, psize, fr, r;
    if (dev->audio.running || dev->sys_audio)
        return 0;
#ifdef __linux__
    /* Linux claims the interface by unbinding snd-usb-audio; use the ALSA card
     * while it exists, so the sound server keeps seeing the device. */
    if (start_system_audio(dev, cb, user, rate) == 0)
        return 0;
#endif
    if ((r = pick_audio_alt(dev, &alt, &psize, &fr)) == 0 && !dev->audio_claimed) {
        if ((r = libusb_claim_interface(dev->h, AUDIO_IFACE)) == 0)
            dev->audio_claimed = 1;
    }
    if (r) {
        /* The OS audio driver owns the interface (the black Dazzle on macOS,
         * any box on Windows): record from its input device instead. */
        if (start_system_audio(dev, cb, user, rate) == 0)
            return 0;
        dbg(0, "cannot open the audio interface: %s", libusb_strerror(r));
        return r;
    }
    dev->audio_cb = cb;
    dev->audio_user = user;
    if (rate)
        *rate = fr;
    dbg(1, "audio on alt %d, %d Hz, %d bytes/packet", alt, fr, psize);
    /* one packet per microframe: 64 packets = 8 ms per transfer */
    return stream_start(dev, &dev->audio, alt, psize, NUM_PKTS);
}

void dvc100_audio_stop(dvc100_t *dev)
{
    if (dev->sys_audio) {
        sa_input_stop(dev->sys_audio);
        dev->sys_audio = NULL;
    }
    stream_stop(dev, &dev->audio);
}
