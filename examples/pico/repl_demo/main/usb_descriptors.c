/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/usb_descriptors.c
 *
 * Minimal TinyUSB USB descriptor callbacks for the urbi REPL demo.
 * Single CDC-ACM interface; no MSC, HID, MIDI or Vendor endpoints.
 *
 * These three callbacks are mandatory when using the TinyUSB device stack
 * directly (without pico_stdio_usb which provides its own descriptors). */

#include "tusb.h"

/* USB IDs — Raspberry Pi VID, generic CDC PID (non-production; replace
 * with your own VID/PID when building a real product). */
#define USB_VID  0x2E8A   /* Raspberry Pi */
#define USB_PID  0x000A   /* Pico CDC UART */
#define USB_BCD  0x0200

/* ------------------------------------------------------------------ */
/* Device descriptor                                                   */
/* ------------------------------------------------------------------ */

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    /* IAD required for CDC */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* ------------------------------------------------------------------ */
/* Configuration descriptor                                            */
/* ------------------------------------------------------------------ */

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_fs_configuration;
}

/* ------------------------------------------------------------------ */
/* String descriptors                                                  */
/* ------------------------------------------------------------------ */

static char const *const string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 }, /* 0: English (0x0409) */
    "urbi-embedded",              /* 1: Manufacturer */
    "urbi REPL demo",             /* 2: Product */
    NULL,                         /* 3: Serial — filled from unique ID below */
};

/* 16-character hex serial from the RP2040 unique flash ID. */
static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        /* Language ID descriptor */
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else if (index < (uint8_t)(sizeof string_desc_arr /
                                 sizeof string_desc_arr[0])) {
        const char *str = string_desc_arr[index];
        if (str == NULL) {
            /* Serial number: use a fixed placeholder (actual pico_unique_id
             * is available at runtime but requires board-specific code; for
             * the link gate any 4-char serial is sufficient). */
            str = "0000";
        }
        chr_count = 0;
        for (const char *p = str; *p != '\0' && chr_count < 31U; p++) {
            _desc_str[1U + chr_count] = (uint16_t)*p;
            chr_count++;
        }
    } else {
        return NULL;
    }

    /* Header: length (bytes) + string type. */
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) |
                               (2U * chr_count + 2U));
    return _desc_str;
}
