/*
 * dvc100 - userspace driver for the Dazzle DVC100 (eMPIA em28xx + SAA7113)
 *
 * Pure libusb, no kernel driver. Video comes from the vendor-class
 * interface 0 (isochronous endpoint 0x82). Audio comes from the USB Audio
 * Class streaming interface 2 (endpoint 0x83) when it can be claimed, and
 * otherwise from the operating system's audio input device for the box.
 * Runs on macOS, Linux and Windows (interface 0 bound to WinUSB).
 */
#ifndef DVC100_H
#define DVC100_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dvc100 dvc100_t;

/* Errors are negative: a libusb_error value, or one of these. */
#define DVC100_ERR_NO_DEVICE (-1001)
#define DVC100_ERR_I2C       (-1002)
#define DVC100_ERR_TIMEOUT   (-1003)
#define DVC100_ERR_NO_MEMORY (-1004)

typedef enum {
    DVC100_INPUT_COMPOSITE = 0,
    DVC100_INPUT_SVIDEO = 1,
} dvc100_input_t;

typedef enum {
    DVC100_NORM_AUTO = 0,
    DVC100_NORM_NTSC = 1, /* 525 lines / 59.94 Hz -> 720x480 */
    DVC100_NORM_PAL = 2,  /* 625 lines / 50 Hz    -> 720x576 */
} dvc100_norm_t;

typedef struct {
    const uint8_t *data;   /* interleaved YUYV (Y0 Cb Y1 Cr), stride = width*2 */
    int width;
    int height;
    uint64_t sequence;
    int top_field_first;   /* 1 if the first field received was the top field */
} dvc100_frame_t;

typedef void (*dvc100_frame_cb)(const dvc100_frame_t *frame, void *user);

typedef struct {
    uint8_t chip_id;       /* em28xx reg 0x0a */
    uint8_t chip_cfg;      /* em28xx reg 0x00 */
    uint8_t decoder_addr;  /* 8-bit I2C address of the SAA711x */
    uint8_t decoder_ver;   /* SAA711x reg 0x00 */
    uint8_t decoder_status;/* SAA711x reg 0x1f */
    int integrated_decoder; /* em2828x: decoder inside the bridge */
    int has_ac97;
    int vendor_audio;
    int alt_setting;
    int iso_packet_size;
} dvc100_info_t;

/* Open the first matching device (vid/pid of 0 = built-in list). */
int dvc100_open(dvc100_t **out, uint16_t vid, uint16_t pid);
void dvc100_close(dvc100_t *dev);

/* Power up the bridge, decoder and audio path. */
int dvc100_init(dvc100_t *dev, dvc100_input_t input, dvc100_norm_t norm);
int dvc100_get_info(dvc100_t *dev, dvc100_info_t *info);
dvc100_norm_t dvc100_current_norm(dvc100_t *dev);
int dvc100_set_input(dvc100_t *dev, dvc100_input_t input);

/* Picture controls, 0..255 with 128 = default. */
int dvc100_set_brightness(dvc100_t *dev, int v);
int dvc100_set_contrast(dvc100_t *dev, int v);
int dvc100_set_saturation(dvc100_t *dev, int v);
int dvc100_set_hue(dvc100_t *dev, int v);

/* Streaming: start launches an event thread that invokes cb per frame. */
int dvc100_start(dvc100_t *dev, dvc100_frame_cb cb, void *user);
void dvc100_stop(dvc100_t *dev);

/* Audio: interleaved signed 16-bit stereo; rate (48000) is returned via *rate.
 * Runs independently of video on the same event thread. */
typedef void (*dvc100_audio_cb)(const int16_t *samples, int frames, void *user);
int dvc100_audio_start(dvc100_t *dev, dvc100_audio_cb cb, void *user, int *rate);
void dvc100_audio_stop(dvc100_t *dev);

/* Low level helpers, exposed for debugging tools. */
int dvc100_reg_read(dvc100_t *dev, uint16_t reg, uint8_t *buf, int len);
int dvc100_reg_write(dvc100_t *dev, uint16_t reg, const uint8_t *buf, int len);
int dvc100_i2c_write(dvc100_t *dev, uint8_t addr, const uint8_t *buf, int len);
int dvc100_i2c_read_reg(dvc100_t *dev, uint8_t addr, uint8_t reg, uint8_t *val);

const char *dvc100_strerror(int err);
void dvc100_set_verbose(int level);

/*
 * Field assembler, separate from USB so it can be unit tested.
 * Feed it the payload of each isochronous packet in order.
 */
typedef struct {
    int width, height;          /* full frame size */
    uint8_t *frame;             /* width*height*2 */
    size_t field_off;           /* bytes written into the current field */
    int cur_top;                /* current field is the top field */
    int have_field;             /* inside a field */
    int got_top, got_bottom;
    int first_top;
    uint64_t sequence;
    uint64_t dropped;
    dvc100_frame_cb cb;
    void *user;
} dvc100_assembler_t;

int dvc100_asm_init(dvc100_assembler_t *a, int width, int height,
                    dvc100_frame_cb cb, void *user);
void dvc100_asm_free(dvc100_assembler_t *a);
void dvc100_asm_packet(dvc100_assembler_t *a, const uint8_t *p, size_t len);

#ifdef __cplusplus
}
#endif
#endif
