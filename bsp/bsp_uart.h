#ifndef __BSP_UART_H
#define __BSP_UART_H

#include <stddef.h>

/**
 * @file bsp_uart.h
 * @brief UART 日志后端（STM32 HAL，USART1 / PA9-TX PA10-RX）
 *
 * 为 modules/component/gen_log 的 GEN_LOG_PORT_UART 端口提供输出函数 bsp_uart_log()。
 * gen_log_port.h 里已声明 extern int bsp_uart_log(const char *fmt, ...); 本文件实现它。
 * printf 风格格式化(vsnprintf)后，经 HAL_UART_Transmit 从 USART1(PA9/PA10, 115200) 发出。
 * USART1 由 CubeMX 的 MX_USART1_UART_Init() 初始化（main() 先于 app_main_init）。
 */

/* gen_log 的 UART 端口输出函数（printf 风格，阻塞发送）。返回写入字节数。 */
int bsp_uart_log(const char *fmt, ...);

#endif /* __BSP_UART_H */
