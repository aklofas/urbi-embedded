/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/tusb_config.h
 *
 * TinyUSB configuration for the urbi REPL demo.  Required when linking
 * tinyusb_device directly (not via pico_stdio_usb) — the pico_stdio_usb
 * tusb_config.h suppresses its own definitions when LIB_TINYUSB_DEVICE=1,
 * so applications that manage the CDC interface themselves must provide
 * this file.
 *
 * CFG_TUSB_MCU and CFG_TUSB_OS are supplied by CMake -D flags; all other
 * required knobs are set here. */

#ifndef _URBI_REPL_DEMO_TUSB_CONFIG_H_
#define _URBI_REPL_DEMO_TUSB_CONFIG_H_

/* CFG_TUSB_MCU must be defined by the build system (-DCFG_TUSB_MCU=...). */
#ifndef CFG_TUSB_MCU
#  error CFG_TUSB_MCU must be defined (set by pico-sdk CMake)
#endif

#ifndef CFG_TUSB_OS
#  define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#  define CFG_TUSB_DEBUG 0
#endif

/* Enable the device stack on RHPORT 0 (RP2040 has one USB port). */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     OPT_MODE_DEFAULT_SPEED

#ifndef CFG_TUSB_MEM_SECTION
#  define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#  define CFG_TUSB_MEM_ALIGN  __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#  define CFG_TUD_ENDPOINT0_SIZE 64
#endif

/* Class drivers: CDC only; everything else off. */
#define CFG_TUD_CDC      1
#define CFG_TUD_MSC      0
#define CFG_TUD_HID      0
#define CFG_TUD_MIDI     0
#define CFG_TUD_VENDOR   0

#define CFG_TUD_CDC_RX_BUFSIZE   64
#define CFG_TUD_CDC_TX_BUFSIZE   64
#define CFG_TUD_CDC_EP_BUFSIZE   64

#endif /* _URBI_REPL_DEMO_TUSB_CONFIG_H_ */
