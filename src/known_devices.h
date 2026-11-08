/* USB IDs of the supported capture boxes, shared by the driver and the Windows driver installer. */
#ifndef DVC100_KNOWN_DEVICES_H
#define DVC100_KNOWN_DEVICES_H

#include <stdint.h>

static const struct {
    uint16_t vid, pid;
    const char *name;
} known_devices[] = {
    { 0x2304, 0x021a, "Dazzle DVC100" },
    { 0x2304, 0x0207, "Dazzle DVC90" },
    /* em2828x family (chip id 0x9x) with the decoder built into the bridge,
     * from embda_x86_x64.inf ("Roxio/Dazzle Video Capture USB") */
    { 0x1b80, 0xe60a, "Dazzle Video Capture USB" },
    { 0x1b80, 0xe31d, "Roxio Video Capture USB" },
    { 0x1b80, 0xe31c, "Roxio Video Capture USB" },
};

#define KNOWN_DEVICE_COUNT (sizeof(known_devices) / sizeof(known_devices[0]))

#endif
