/**
 * @file gen_log.h
 * @brief 跨平台模块化日志接口（编译期开关）
 *
 * 设计目标:
 *   - 模块代码不直接调用 printf，统一走 gen_log_xxx，保证可移植到任意嵌入式平台；
 *   - 平台差异收敛到 gen_log_port.h（换平台只改那一处）；
 *   - 两层开关：全局 GEN_LOG_ENABLE + 每模块 GEN_LOG_MODULE；
 *   - 关闭时宏退化为 ((void)0)，编译器彻底消除字符串与调用，不占 Flash。
 *
 * 使用方法（在需要打印的 .c 文件顶部）:
 *     #define GEN_LOG_MODULE  GEN_LOG_DISPLAY   // 引用 gen_log_cfg.h 里的开关
 *     #define GEN_LOG_TAG     "display"        // 日志前缀标签（必须是字符串字面量）
 *     #include "gen_log.h"
 *
 *     gen_log_info("init ok, w=%d\n", width);  // -> [I][display] init ok, w=128
 *
 * 断言（致命错误，不受模块开关控制，仅受全局开关约束）:
 *     gen_log_assert("null ptr in %s\n", __func__);
 *
 * 注意: 宏本身不追加换行，调用方在 fmt 中自行控制 '\n'（与原 printf 行为一致，
 *       也方便 hex dump 这类连续打印场景）。
 */
#ifndef GEN_LOG_H
#define GEN_LOG_H

#include "gen_log_cfg.h"   /* 全局开关 + 各模块开关 */
#include "gen_log_port.h"  /* 平台输出端口：GEN_LOG_RAW(fmt, ...) */

/* ---- 全局大开关兜底（通常由 gen_log_cfg.h 或 CMake 提供）---- */
#ifndef GEN_LOG_ENABLE
#define GEN_LOG_ENABLE 1
#endif

/* ---- 当前模块开关：由调用 .c 在 include 前用 #define GEN_LOG_MODULE 指定 ---- */
#ifndef GEN_LOG_MODULE
#define GEN_LOG_MODULE 1
#endif

/* ---- 当前模块标签：由调用 .c 指定，未指定时用 "*" ---- */
#ifndef GEN_LOG_TAG
#define GEN_LOG_TAG "*"
#endif

/* ============================================================
 * 两层判定：全局开 且 本模块开，才真正编译进代码
 *   GEN_LOG_ENABLE=0           -> 全部模块静音
 *   GEN_LOG_ENABLE=1, 模块=0   -> 仅该模块静音
 * ============================================================ */
#define GEN_LOG_ON (GEN_LOG_ENABLE && GEN_LOG_MODULE)

#if GEN_LOG_ON
#define gen_log_err(fmt, ...)    GEN_LOG_RAW("[E][" GEN_LOG_TAG "] " fmt, ##__VA_ARGS__)
#define gen_log_warn(fmt, ...)   GEN_LOG_RAW("[W][" GEN_LOG_TAG "] " fmt, ##__VA_ARGS__)
#define gen_log_info(fmt, ...)   GEN_LOG_RAW("[I][" GEN_LOG_TAG "] " fmt, ##__VA_ARGS__)
#define gen_log_dbg(fmt, ...)    GEN_LOG_RAW("[D][" GEN_LOG_TAG "] " fmt, ##__VA_ARGS__)
#else
#define gen_log_err(fmt, ...)    ((void)0)
#define gen_log_warn(fmt, ...)   ((void)0)
#define gen_log_info(fmt, ...)   ((void)0)
#define gen_log_dbg(fmt, ...)    ((void)0)
#endif

/* ---- 断言：致命错误，不受模块开关控制，始终输出（仅受全局开关约束）---- */
#if GEN_LOG_ENABLE
#define gen_log_assert(fmt, ...) GEN_LOG_RAW("[A][" GEN_LOG_TAG "] " fmt, ##__VA_ARGS__)
#else
#define gen_log_assert(fmt, ...) ((void)0)
#endif

#endif /* GEN_LOG_H */
