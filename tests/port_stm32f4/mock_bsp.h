/* SPDX-License-Identifier: BSD-3-Clause */
/* Mock BSP layer for host-side unit tests of STM32F4 port shims.
 *
 * Replaces the STMicroelectronics BSP_* / HAL_* / __get_IPSR symbols with
 * host-runnable fakes that capture call args into per-test buffers.  Each
 * test resets the mock state with mock_bsp_reset() before exercising
 * the port shim under test.
 *
 * This header MUST be included BEFORE the port shim TU under test,
 * so the macros below shadow the BSP symbols at compile time. */
#ifndef URBI_TEST_MOCK_BSP_H
#define URBI_TEST_MOCK_BSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- LCD mock ---- */

typedef struct {
    int call_count;
    uint16_t last_x, last_y, last_w, last_h;
    uint32_t last_color;
} MockLcdState;

extern MockLcdState mock_lcd;

void mock_BSP_LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void mock_BSP_LCD_SetTextColor(uint32_t color);
uint8_t mock_BSP_LCD_Init(void);

#define BSP_LCD_FillRect       mock_BSP_LCD_FillRect
#define BSP_LCD_SetTextColor   mock_BSP_LCD_SetTextColor
#define BSP_LCD_Init           mock_BSP_LCD_Init

/* ---- Gyro mock ---- */

typedef struct {
    int call_count;
    float fake_xyz[3];  /* set by tests; mock_BSP_GYRO_GetXYZ writes these to out */
} MockGyroState;

extern MockGyroState mock_gyro;

void mock_BSP_GYRO_GetXYZ(float *out);
uint8_t mock_BSP_GYRO_Init(void);

#define BSP_GYRO_GetXYZ  mock_BSP_GYRO_GetXYZ
#define BSP_GYRO_Init    mock_BSP_GYRO_Init

/* ---- UART mock ---- */

typedef struct {
    int call_count;
    char buf[4096];     /* capture all bytes transmitted */
    size_t buf_len;
} MockUartState;

extern MockUartState mock_uart;

typedef struct { int dummy; } UART_HandleTypeDef;
extern UART_HandleTypeDef mock_uart_handle;

int mock_HAL_UART_Transmit(UART_HandleTypeDef *huart, const uint8_t *data,
                           uint16_t size, uint32_t timeout);

#define HAL_UART_Transmit  mock_HAL_UART_Transmit
#define huart1             mock_uart_handle  /* port_writer.c references huart1 */

/* ---- ISR-context mock ---- */

extern uint32_t mock_ipsr;
uint32_t mock_get_ipsr(void);

#define __get_IPSR  mock_get_ipsr

/* ---- DWT cycle-counter mock ---- */

extern uint32_t mock_dwt_cyccnt;
extern uint32_t mock_dwt_ctrl;

/* mock_get_dwt returns mock_dwt_cyccnt; tests can advance it. */
uint32_t mock_get_dwt(void);

/* port_time.c reads DWT->CYCCNT directly; the mock substitutes a function call
 * via the macro below. */
#define DWT_CYCCNT_READ()  mock_get_dwt()

/* ---- Event-ring capture (for test_port_button.c) ---- */

typedef struct {
    int call_count;
    uint32_t last_event_id;
    void *last_vm;
} MockEventRingState;

extern MockEventRingState mock_event_ring;

/* port_button.c calls this from its EXTI handler — we intercept it here. */
int mock_urbi_event_emit_from_isr(void *vm, uint32_t event_id, void *payload);

#define urbi_event_emit_from_isr  mock_urbi_event_emit_from_isr

/* ---- Reset all mock state to zero (call from each test setup) ---- */

void mock_bsp_reset(void);

#endif /* URBI_TEST_MOCK_BSP_H */
