/**
 * @file gen_log_cfg.h
 * @brief 日志开关集中配置（项目方维护）
 *
 * 两层语义：
 *   - GEN_LOG_ENABLE 是全局大开关；关掉则所有日志（含 assert）全部静音。
 *   - 各 GEN_LOG_xxx 是模块开关，仅在全局打开时生效，可单独关掉某个模块。
 *
 * 配置方式（外部 -D 优先级高于本文件默认值）：
 *   1. 直接修改本文件中的 0/1；
 *   2. 通过编译选项覆盖——所有开关均有 #ifndef 保护：
 *        全局开关由 CMake 选项注入：  cmake -DGEN_LOG_ENABLE=OFF ..
 *        单模块开关用编译定义注入，例如：
 *        cmake -DCMAKE_C_FLAGS=-DGEN_LOG_DISPLAY=0 ..
 *        或在 CMakeLists.txt 里 add_compile_definitions(GEN_LOG_DISPLAY=0)。
 */
#ifndef GEN_LOG_CFG_H
#define GEN_LOG_CFG_H

/* ---- 全局大开关：0=关闭所有日志（含 assert），1=打开 ---- */
#ifndef GEN_LOG_ENABLE
#define GEN_LOG_ENABLE 1
#endif

/* ---- 各模块独立开关（全局打开时才生效）：1=开 0=关 ---- */
/* 组件层 */
#ifndef GEN_LOG_FONT
#define GEN_LOG_FONT         1   /* font_conver */
#endif
#ifndef GEN_LOG_MALLOC
#define GEN_LOG_MALLOC       1   /* gen_malloc */
#endif
#ifndef GEN_LOG_SF
#define GEN_LOG_SF           1   /* sf 状态机框架（assert 输出） */
#endif

/* 服务层 */
#ifndef GEN_LOG_DISPLAY
#define GEN_LOG_DISPLAY      1   /* svc_display */
#endif

/* 模拟层（仅 PC sim 构建会用到） */
#ifndef GEN_LOG_SIM_LCD
#define GEN_LOG_SIM_LCD      1   /* sim_lcd */
#endif
#ifndef GEN_LOG_SIM_RF
#define GEN_LOG_SIM_RF       1   /* sim_rf */
#endif
#ifndef GEN_LOG_SIM_FONT
#define GEN_LOG_SIM_FONT     1   /* sim_font */
#endif
#ifndef GEN_LOG_SIM_SOCK
#define GEN_LOG_SIM_SOCK     1   /* unix_socket */
#endif

#endif /* GEN_LOG_CFG_H */
