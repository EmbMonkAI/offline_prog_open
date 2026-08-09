/**
 * @file gen_log_port.h
 * @brief 日志输出平台端口（平台相关）
 *
 * 换平台时只需在此选择对应的 GEN_LOG_RAW 实现，上层模块代码无需改动。
 * 通过编译宏选择端口（由根 CMakeLists.txt 的 GEN_LOG_PORT 注入）:
 *   GEN_LOG_PORT_SIM   : PC 模拟（printf）      —— sim 构建默认
 *   GEN_LOG_PORT_RTT   : SEGGER RTT             —— 硬件调试常用
 *   GEN_LOG_PORT_UART  : 板载 UART（走 bsp）    —— 量产
 *
 * 未指定任何端口时默认走 SIM，保证 PC 编译总有输出。
 *
 * 新增平台端口：在此追加一个 #elif 分支并提供 GEN_LOG_RAW 即可。
 */
#ifndef GEN_LOG_PORT_H
#define GEN_LOG_PORT_H

/* 未显式指定端口时，默认按 PC 模拟处理 */
#if !defined(GEN_LOG_PORT_SIM) && !defined(GEN_LOG_PORT_RTT) && !defined(GEN_LOG_PORT_UART)
#define GEN_LOG_PORT_SIM 1
#endif

/* ---- PC 模拟：标准 printf ---- */
#ifdef GEN_LOG_PORT_SIM
#include <stdio.h>
#define GEN_LOG_RAW(fmt, ...)  printf(fmt, ##__VA_ARGS__)
#endif

/* ---- SEGGER RTT ---- */
#ifdef GEN_LOG_PORT_RTT
#include "SEGGER_RTT.h"
#define GEN_LOG_RAW(fmt, ...)  SEGGER_RTT_printf(0, fmt, ##__VA_ARGS__)
#endif

/* ---- 板载 UART：交给 bsp 实现（bsp 内部做 vsnprintf + DMA/发送）----
 * 硬件量产使用时，解开下方注释并改成你 bsp 中实际的头文件与输出函数。
 */
#ifdef GEN_LOG_PORT_UART
/* #include "bsp_uart.h" */
extern int bsp_uart_log(const char *fmt, ...);   /* 按 bsp 实际签名调整 */
#define GEN_LOG_RAW(fmt, ...)  bsp_uart_log(fmt, ##__VA_ARGS__)
#endif

#endif /* GEN_LOG_PORT_H */
