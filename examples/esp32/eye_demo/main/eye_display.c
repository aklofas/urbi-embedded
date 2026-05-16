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

/* display_task_body, display_post_frame, draw_crosshair_into and
 * c_draw_crosshair land in T31-T34 commits. */
static void display_task_body(void *arg)
{
    (void)arg;
    /* T31 body lands next commit. */
    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
