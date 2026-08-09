/**
 * @file dev_font.c
 * @brief 字体数据读取设备驱动实现（真实硬件：SD 卡 FATFS）
 *
 * 从 SD 卡根目录读取字体文件 "kp_font_lib.bin"（与 PC 模拟 sim_font.c 同一文件），
 * 供 font_mgr 模块按地址读取点阵数据。
 *
 * 前提：调用 dev_font_init() 前卡已 f_mount（app_main 的 fatfs_test 已挂载）。
 * PC 模拟时链接 sim/sim_font.c 替代本文件。
 */
#include "dev_font.h"

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "dev-font"
#include "gen_log.h"
#include <string.h>
#include "ff.h"                /* FatFs（CubeMX 生成） */

/** 字体文件名（SD 卡根目录） */
#define FONT_FILE_NAME "0:kp_font_lib.bin"

/** 字体文件句柄（静态，避免占栈；单例，不支持多字体并发） */
static FIL s_font_file;
static uint8_t s_font_open = 0;   /* 1=文件已打开 */

int dev_font_init(void)
{
    FRESULT fr;

    if (s_font_open)
        return 0;   /* 已初始化（幂等） */

    fr = f_open(&s_font_file, FONT_FILE_NAME, FA_READ);
    if (fr != FR_OK) {
        /* 字库缺失不算致命：色块/线条照画，只是文字渲染取不到点阵 */
        gen_log_info("font file not found (%d): %s\n", (int)fr, FONT_FILE_NAME);
        return -1;
    }

    s_font_open = 1;
    gen_log_info("font load OK: %s (%lu B)\n", FONT_FILE_NAME,
                 (unsigned long)f_size(&s_font_file));
    return 0;
}

uint8_t dev_font_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    FRESULT fr;
    UINT br = 0;

    if (!s_font_open || !buf)
        return 1;

    fr = f_lseek(&s_font_file, addr);
    if (fr != FR_OK)
        return 1;

    fr = f_read(&s_font_file, buf, len, &br);
    if (fr != FR_OK || br != len)
        return 1;

    return 0;
}

void dev_font_deinit(void)
{
    if (s_font_open) {
        f_close(&s_font_file);
        s_font_open = 0;
    }
}
