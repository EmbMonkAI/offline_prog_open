/**
 * @file test_display.c
 * @brief LCD显示测试程序
 *
 * 只使用 dev_lcd.h + svc_display.h 接口，完全不感知底层实现。
 * 编译时链接不同驱动（模拟或真实），应用代码无需修改。
 */

#include "dev_lcd.h"
#define GEN_LOG_TAG     "test"
#include "gen_log.h"
#include "svc_display.h"
#include "font_mgr.h"
#include "dev_font.h"
#include "kp_font_lib.h"
#include <stdio.h>

/* 外部LCD设备：编译时由驱动层提供（sim_lcd.c 或真实驱动） */
extern dev_lcd_dev_t g_lcd_dev;

/* ==================== 字体对象（ASCII 8x16） ==================== */

static font_mgr_t s_font_8x16 = {
    .encoding  = FONT_MGR_ENCODING_ASCII,
    .width     = 8,
    .height    = 16,
    .base_addr = FONTS_ASCII_8X16_ADDR,
    .read      = dev_font_read,
};

/* ==================== 字体对象（GB2312 中文 16x16，含 ASCII 回退） ==================== */
/* 中文走 GB2312 索引（base=FONTS_CHS_16X16_ADDR），ASCII 字符由 font_mgr 回退
 * 到 8x16（恒定 16 字节）。验证单色 LCD 既能显示英文也能显示中文，而非仅色块。 */
static font_mgr_t s_font_chs_16x16 = {
    .encoding  = FONT_MGR_ENCODING_GB2312,
    .width     = 16,
    .height    = 16,
    .base_addr = FONTS_CHS_16X16_ADDR,
    .read      = dev_font_read,
};

/* ==================== 中文串（GBK 原始字节） ==================== */
/* 源文件是 UTF-8，故不直接写中文字面量（避免源码编码影响字节内容）。
 * font_mgr 的 GB2312 路径读取的是 GBK/GB2312 双字节码，这里用显式字节表达，
 * 与 PC/真屏字库的字节序一一对应，不依赖编译器源码编码。
 *   "脱机编程器" = CD D1 BB FA B1 E0 B3 CC C6 F7
 *   "中文显示" = D6 D0 CE C4 CF D4 CA BE
 *   "English"  = 走 GB2312 字体的 ASCII 回退（8x16）
 */
static const uint8_t s_str_device[]   = {0xCD,0xD1,0xBB,0xFA,0xB1,0xE0,0xB3,0xCC,0xC6,0xF7,0x00};
static const uint8_t s_str_chinese[]  = {0xD6,0xD0,0xCE,0xC4,0xCF,0xD4,0xCA,0xBE,0x00};

/* ==================== 测试用例（纯 dev_lcd / svc_display 接口） ==================== */

static void test_clear(void)
{
    svc_display_clear(SVC_DISPLAY_BUF_BASE);
    svc_display_clear(SVC_DISPLAY_BUF_TOP);
    svc_display_refresh(SVC_DISPLAY_BUF_BASE);
    svc_display_refresh(SVC_DISPLAY_BUF_TOP);
}

static void test_line(void)
{
    svc_display_line_t xline = {{0, 19}, SVC_DISPLAY_WIDTH};
    svc_display_x_line(SVC_DISPLAY_BUF_BASE, &xline, SVC_DISPLAY_COLOR_FRG);

    // svc_display_line_t yline = {{64, 0}, SVC_DISPLAY_HEIGHT/2};
    // svc_display_y_line(SVC_DISPLAY_BUF_BASE, &yline, SVC_DISPLAY_COLOR_FRG);

    // svc_display_refresh(SVC_DISPLAY_BUF_BASE);
}

static void test_area_pixel(void)
{
    svc_display_area_t area = {10, 24, 6, 8};
    uint8_t data[] = {0x7E, 0x10, 0x10, 0x10, 0x10, 0x00};
    svc_display_area_pixel(SVC_DISPLAY_BUF_BASE, &area, data, SVC_DISPLAY_COLOR_FRG);

    // svc_display_refresh(SVC_DISPLAY_BUF_BASE);
}

static void test_rect_text(void)
{
    svc_display_rect_t rect = {{10, 0, 108, 16}, 0, 0};
    svc_display_rect_text(SVC_DISPLAY_BUF_BASE, &rect,
                           (const uint8_t *)"Hello LCD",
                           &s_font_8x16,
                           SVC_DISPLAY_ALIGN_CENTER,
                           SVC_DISPLAY_COLOR_FRG, 1);

    // svc_display_refresh(SVC_DISPLAY_BUF_BASE);
}

/* 单色 LCD：第 1 行英文标题（ASCII 8x16） */
static void draw_title(void)
{
    svc_display_rect_t rect = {{0, 0, SVC_DISPLAY_WIDTH, 16}, 0, 0};
    svc_display_rect_text(SVC_DISPLAY_BUF_BASE, &rect,
                          (const uint8_t *)"Offline PROG",
                          &s_font_8x16,
                          SVC_DISPLAY_ALIGN_CENTER,
                          SVC_DISPLAY_COLOR_FRG, 1);
}

/* 单色 LCD：第 2 行中文「脱机编程器」（GB2312 16x16） */
static void draw_chs_device(void)
{
    svc_display_rect_t rect = {{0, 18, SVC_DISPLAY_WIDTH, 16}, 0, 0};
    svc_display_rect_text(SVC_DISPLAY_BUF_BASE, &rect,
                          s_str_device,
                          &s_font_chs_16x16,
                          SVC_DISPLAY_ALIGN_CENTER,
                          SVC_DISPLAY_COLOR_FRG, 1);
}

/* 单色 LCD：第 3 行中文「中文显示」（GB2312 16x16） */
static void draw_chs_chinese(void)
{
    svc_display_rect_t rect = {{0, 40, SVC_DISPLAY_WIDTH, 16}, 0, 0};
    svc_display_rect_text(SVC_DISPLAY_BUF_BASE, &rect,
                          s_str_chinese,
                          &s_font_chs_16x16,
                          SVC_DISPLAY_ALIGN_CENTER,
                          SVC_DISPLAY_COLOR_FRG, 1);
}

static void test_rect_fill(void)
{
    svc_display_rect_t rect = {{80, 40, 40, 20}, 0, 0};
    svc_display_rect_fill_all(SVC_DISPLAY_BUF_BASE, &rect, SVC_DISPLAY_COLOR_FRG);

    svc_display_refresh(SVC_DISPLAY_BUF_BASE);
}



/* ==================== 主函数 ==================== */

int main(void)
{
    dev_lcd_init(&g_lcd_dev);
    svc_display_init(&g_lcd_dev);

    test_clear();

    /* 第 1 行：英文（ASCII 8x16） */
    draw_title();
    /* 第 2、3 行：中文（GB2312 16x16，验证字库渲染，非仅色块） */
    draw_chs_device();
    draw_chs_chinese();

    svc_display_refresh(SVC_DISPLAY_BUF_BASE);

    gen_log_info("test passed, close window or press Ctrl+C to quit\n");
    while (1) {}

    svc_display_deinit();
    dev_lcd_deinit(&g_lcd_dev);
    return 0;
}
