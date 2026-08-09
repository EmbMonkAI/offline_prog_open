/**
 * @file bsp_uart.c
 * @brief UART 日志后端实现（STM32 HAL，USART1 / PA9-TX PA10-RX）
 *
 * vsnprintf 格式化 -> HAL_UART_Transmit 阻塞发送。供 gen_log 的 UART 端口使用。
 * USART1 走 CubeMX（MX_USART1_UART_Init，115200-8N1，main() 里先于 app_main_init
 * 执行），本文件只发送不初始化。V3 板日志口即 UART1（2026-08-28 由 USART2 切回）。
 */

#include "bsp_uart.h"
#include "stm32f4xx_hal.h"
#include <stdarg.h>
#include <stdio.h>

/* CubeMX 生成的 USART1 句柄（定义在 main.c） */
extern UART_HandleTypeDef huart1;

#define BSP_UART_LOG_BUF_SZ   160   /* 单条日志最大长度（超出截断） */
#define BSP_UART_TIMEOUT_MS   100

int bsp_uart_log(const char *fmt, ...)
{
    char buf[BSP_UART_LOG_BUF_SZ];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) return n;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);   /* 被截断 */

    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, BSP_UART_TIMEOUT_MS);
    return n;
}
