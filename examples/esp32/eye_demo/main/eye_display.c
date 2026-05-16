/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_display — ST7789 240x240 LCD driver via SPI2.
 *
 * Hardware: ESP32-S3-EYE v2.2.  SCLK=21, MOSI=47, DC=43, CS=44, BL=48.
 * 40 MHz pclk, 16 bpp, SPI mode 0, queue depth 10.  Backlight pin is
 * defined here for future use; the spec/plan leave it untouched on the
 * grounds that the S3-EYE board pulls BL high by default and the demo
 * doesn't need to drive it.
 *
 * This TU is target-only.  The crosshair-overlay primitive
 * (draw_crosshair_into) is split into the sibling crosshair.h header so
 * the host unit test in tests/unit/test_draw_crosshair.c can drive it
 * without ESP-IDF dependencies. */

#include <string.h>      /* memcpy — used by the centre-crop in display_task_body */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_camera.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"

#include "urbi/urbi.h"

#include "eye_display.h"
#include "crosshair.h"

/* === ESP32-S3-EYE v2.2 pin map (see board schematic) === */
#define LCD_HOST       SPI2_HOST
#define LCD_PIN_SCLK   21
#define LCD_PIN_MOSI   47
#define LCD_PIN_DC     43
#define LCD_PIN_CS     44
#define LCD_PIN_RST    -1
#define LCD_PIN_BL     48

/* === Display task state ===
 *
 * lcd / frame_q are written once by eye_display_init before the task
 * starts, then read by display_task_body.  No synchronisation needed.
 *
 * crosshair_x / crosshair_y are volatile because they are written from
 * the urbi VM task (via c_draw_crosshair) and read from the display task
 * without an explicit fence.  Aligned 32-bit writes on the Xtensa LX7 are
 * atomic, but writing TWO ints isn't atomic as a pair — there is a brief
 * window where x updates before y.  We tolerate this tearing: it is
 * visually invisible at 30 Hz refresh and adding a mutex on the display
 * hot path would cost more than the artefact is worth.  See T34 commit
 * subject ("volatile pair write, atomic on LX7") for the precise
 * semantic. */
static esp_lcd_panel_handle_t lcd;
static QueueHandle_t          frame_q;
static volatile int           crosshair_x = -1;
static volatile int           crosshair_y = -1;

/* Forward declaration — display_task_body is defined in T31. */
static void display_task_body(void *arg);

void eye_display_init(void)
{
    /* SPI bus init.  max_transfer_sz sized for one full 240x240 RGB565
     * frame; SPI_DMA_CH_AUTO lets the driver pick a free channel. */
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 240 * (int)sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* Panel IO over SPI: 40 MHz pclk, mode 0, queue depth 10. */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    /* ST7789 panel: 16 bpp, RGB element order.  The spec sketch's
     * `.rgb_endian = LCD_RGB_ENDIAN_RGB` does not match the real ESP-IDF
     * v6.0.1 field — the canonical name is `.rgb_ele_order` of type
     * `lcd_rgb_element_order_t` (see esp_lcd_panel_dev.h:21). */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd, true));

    /* Frame queue (depth 2) + display task pinned to core 1.  Static
     * allocation keeps the task off the heap. */
    frame_q = xQueueCreate(2, sizeof(camera_fb_t *));
    static StackType_t  d_stack[4096 / sizeof(StackType_t)];
    static StaticTask_t d_tcb;
    xTaskCreateStaticPinnedToCore(display_task_body, "lcd", 4096, NULL,
                                  tskIDLE_PRIORITY + 1, d_stack, &d_tcb, 1);
}

void display_post_frame(camera_fb_t *fb)
{
    /* Block until the display task drains a queue slot.  The queue depth
     * is 2 and the display task runs on core 1 in parallel with the
     * camera task on core 0, so under steady-state load this rarely
     * actually blocks; the camera task is willing to wait when it does
     * (the worst-case backpressure here is one frame at 30 Hz ≈ 33 ms). */
    xQueueSend(frame_q, &fb, portMAX_DELAY);
}

static void display_task_body(void *arg)
{
    (void)arg;
    /* Static-allocated 240x240 RGB565 framebuffer (≈115 KB).  4-byte
     * aligned so esp_lcd_panel_draw_bitmap's DMA path doesn't have to
     * realign on every push.  Off the heap — the camera + display tasks
     * combined fit inside the static partitions called out in the
     * footprint cap. */
    static uint16_t lcd_fb[240 * 240] __attribute__((aligned(4)));
    camera_fb_t *fb;
    while (xQueueReceive(frame_q, &fb, portMAX_DELAY) == pdPASS) {
        /* Centre-crop 320x240 → 240x240 (skip 40 px on the left edge of
         * each scanline).  RGB565 is 2 bytes per pixel; pointer arith on
         * uint16_t already accounts for the stride. */
        const uint16_t *src = (const uint16_t *)fb->buf;
        for (int y = 0; y < 240; y++) {
            memcpy(&lcd_fb[y * 240], &src[y * 320 + 40], 240 * sizeof(uint16_t));
        }

        /* Overlay the latest crosshair coordinates (set by c_draw_crosshair
         * on the urbi VM task).  Reads volatile pair non-atomically; the
         * tearing window is one frame and visually invisible. */
        draw_crosshair_into(lcd_fb, 240, 240, crosshair_x, crosshair_y);

        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(lcd, 0, 0, 240, 240, lcd_fb));

        /* Frame buffer ownership: per the contract documented in
         * eye_camera.c:49-53, this task OWNS the fb once it is dequeued
         * and is responsible for returning it to the camera driver's
         * 2-buffer pool.  Skipping this call starves the pool and
         * esp_camera_fb_get returns NULL on the camera task. */
        esp_camera_fb_return(fb);
    }
}

/* Signature note: the host-fn type at <urbi/urbi.h>:295 is
 *
 *     int (*urbi_native_method_fn)(struct UVM *vm, UValue self,
 *                                  UValue *args, uint8_t nargs, UValue *out);
 *
 * The brainstorm spec sketch in §5.3 used a `UStrand *` / `UValue` return
 * convention that does not match the real v0.7.1 surface — corrected here
 * to the canonical urbi_native_method_fn shape (same drift as T28
 * c_set_target_color; see eye_camera.c:104-115).  Wires straight through:
 *
 *     urbi_register(vm, realm, "draw_crosshair", c_draw_crosshair);
 *
 * Argument convention (from spec §5.5 / urbiscript):
 *
 *     draw_crosshair(x, y)
 *
 * where (x, y) are blob centroid coordinates in the original 320x240
 * camera frame (NOT post-crop).  The -40 offset on the X axis converts
 * to the 240x240 cropped display coordinate space; the Y axis is
 * already aligned because the centre-crop only trims the left edge.
 *
 * Concurrency: writes go to two `volatile int` slots shared with the
 * display task on core 1.  Aligned 32-bit writes are atomic on the
 * Xtensa LX7, so each scalar is safe individually; the pair is not
 * atomic and the display task can observe an updated x with the old y
 * (or vice versa) for at most one frame.  Visually invisible at 30 Hz.
 * A mutex on this hot path is rejected — the demo would gain no
 * perceptible image quality and pay for it in jitter. */
int c_draw_crosshair(struct UVM *vm, UValue self,
                     UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;

    if (nargs < 2 || args == NULL) {
        if (out) *out = urbi_make_nil();
        return UEXEC_OK;
    }

    /* Volatile pair write — atomic per scalar on LX7, tearable as a pair.
     * See header comment above for why this is acceptable. */
    crosshair_x = urbi_value_as_int(args[0]) - 40;  /* 320 → 240 crop offset */
    crosshair_y = urbi_value_as_int(args[1]);

    if (out) *out = urbi_make_nil();
    return UEXEC_OK;
}
