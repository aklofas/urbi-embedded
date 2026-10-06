/* SPDX-License-Identifier: BSD-3-Clause */
/* Channel-dispatching writer over USART1 (ST-Link/V2-B VCP).
 *
 * Format on the wire: "[<channel>] <message>" — channel name in brackets,
 * then a space, then the message.  Caller is expected to include their
 * own newline in the message body if they want one.
 *
 * Uses HAL_UART_Transmit with a blocking timeout; sufficient for
 * diagnostics and demo cerr/clog output, NOT for high-throughput
 * streaming. */

#include "port_stm32f4.h"
#include <stdint.h>
#include <stddef.h>

#include "stm32f4xx_hal.h"
extern UART_HandleTypeDef huart1;

void port_writer(void *ud, const char *chan, size_t chan_len,
                 const char *msg, size_t msg_len) {
    (void)ud;

    const uint8_t open_br = (uint8_t)'[';
    const uint8_t close_br = (uint8_t)']';
    const uint8_t space = (uint8_t)' ';

    HAL_UART_Transmit(&huart1, &open_br, 1, 10);
    HAL_UART_Transmit(&huart1, (const uint8_t *)chan, (uint16_t)chan_len, 100);
    HAL_UART_Transmit(&huart1, &close_br, 1, 10);
    HAL_UART_Transmit(&huart1, &space, 1, 10);
    HAL_UART_Transmit(&huart1, (const uint8_t *)msg, (uint16_t)msg_len, 1000);
}

/* Raw bytes with no channel prefix, for the shim's own lines. */
void port_uart_write(const char *s, size_t n) {
    HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)n, 1000);
}

/* port_uart_init lives here too — sets up USART1 PA9/PA10 at 115200 8N1. */
void port_uart_init(void) {
    /* GPIO init: PA9 (TX) and PA10 (RX) in AF7 (USART1) */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {
        .Pin = GPIO_PIN_9 | GPIO_PIN_10,
        .Mode = GPIO_MODE_AF_PP,
        .Pull = GPIO_PULLUP,
        .Speed = GPIO_SPEED_HIGH,
        .Alternate = GPIO_AF7_USART1,
    };
    HAL_GPIO_Init(GPIOA, &gpio);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}
