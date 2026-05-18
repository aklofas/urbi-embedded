/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include <string.h>

MockLcdState mock_lcd;
MockGyroState mock_gyro;
MockUartState mock_uart;
UART_HandleTypeDef mock_uart_handle;
uint32_t mock_ipsr;
uint32_t mock_dwt_cyccnt;
uint32_t mock_dwt_ctrl;
MockEventRingState mock_event_ring;

void mock_BSP_LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    mock_lcd.call_count++;
    mock_lcd.last_x = x;
    mock_lcd.last_y = y;
    mock_lcd.last_w = w;
    mock_lcd.last_h = h;
}

void mock_BSP_LCD_SetTextColor(uint32_t color) {
    mock_lcd.last_color = color;
}

uint8_t mock_BSP_LCD_Init(void) { return 0; }

void mock_BSP_GYRO_GetXYZ(float *out) {
    mock_gyro.call_count++;
    out[0] = mock_gyro.fake_xyz[0];
    out[1] = mock_gyro.fake_xyz[1];
    out[2] = mock_gyro.fake_xyz[2];
}

uint8_t mock_BSP_GYRO_Init(void) { return 0; }

int mock_HAL_UART_Transmit(UART_HandleTypeDef *huart, const uint8_t *data,
                           uint16_t size, uint32_t timeout) {
    (void)huart;
    (void)timeout;
    mock_uart.call_count++;
    size_t copy = size;
    if (mock_uart.buf_len + copy > sizeof mock_uart.buf) {
        copy = sizeof mock_uart.buf - mock_uart.buf_len;
    }
    memcpy(mock_uart.buf + mock_uart.buf_len, data, copy);
    mock_uart.buf_len += copy;
    return 0;
}

uint32_t mock_get_ipsr(void) { return mock_ipsr; }
uint32_t mock_get_dwt(void) { return mock_dwt_cyccnt; }

int mock_urbi_event_emit_from_isr(void *vm, uint32_t event_id, void *payload) {
    (void)payload;
    mock_event_ring.call_count++;
    mock_event_ring.last_event_id = event_id;
    mock_event_ring.last_vm = vm;
    return 0;
}

void mock_bsp_reset(void) {
    memset(&mock_lcd, 0, sizeof mock_lcd);
    memset(&mock_gyro, 0, sizeof mock_gyro);
    memset(&mock_uart, 0, sizeof mock_uart);
    mock_ipsr = 0;
    mock_dwt_cyccnt = 0;
    mock_dwt_ctrl = 0;
    memset(&mock_event_ring, 0, sizeof mock_event_ring);
}
