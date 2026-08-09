/**
 * @file test_tft_sim.c
 * @brief TFT 显示 PC 模拟测试入口（有 main）
 *
 * 复刻 test_display.c 的 PC 模拟模式：
 *   - 用 sim_lcd_tft.c 提供的 g_lcd_tft_dev（SDL 实现 hw 回调）
 *   - 通过 dev_lcd_tft.h / svc_display_tft.h 接口操作，不感知底层是模拟
 *   - 不依赖任何 HAL / ST7789，纯 PC 编译运行
 *
 * 与 test_tft.c（真屏入口）共用 test_tft_draw_pattern() 画图逻辑。
 * 本文件额外测试文字渲染（PC 有 sim_font 字库，真屏 demo 暂不测文字）。
 *
 * 运行：cmake -B build -G Ninja && ninja test_tft && ./build/service/display/test/test_tft
 *
 * 实时刷新（非「照片」）：画完自检画面后进入主循环，周期性重绘动态内容
 * （移动色条 + 帧计数），并读键盘改前景色，演示「窗口随程序运行变化」。
 * 点窗口 X → sim_lcd_tft_should_quit() → 主循环退出 → shutdown() 干净回收，
 * 不在渲染线程里 exit()（那会触发 Windows 报错对话框）。
 */

#include "dev_lcd_tft.h"
#define GEN_LOG_TAG     "test-tft-sim"
#define GEN_LOG_MODULE 1
#include "gen_log.h"
#include "svc_display_tft.h"
#include "font_mgr.h"
#include "dev_font.h"
#include "kp_font_lib.h"
#include "dev_keyboard.h"
#include "test_tft.h"
/* 本文件只用 SDL_Delay；禁用 SDL 的 main 钩子（Windows 上 SDL.h 会把 main
 * 宏定义成 SDL_main(int,char**)），保持 int main(void) 与项目其它测试入口一致。 */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>

/* sim_lcd_tft.c 提供的 PC 模拟设备（SDL 后端） */
extern dev_lcd_tft_dev_t g_lcd_tft_dev;

/* sim_lcd_tft.c 提供的退出控制（SDL_QUIT → 主线程优雅退出） */
extern int  sim_lcd_tft_should_quit(void);
extern void sim_lcd_tft_shutdown(void);
extern void sim_lcd_tft_pump(void);   /* 主循环每帧调用：poll 事件 + 刷新 */

/* ==================== 键盘（PC 模拟：sim_keyboard.c 注入 SDL 键盘事件） ==================== */

static dev_keyboard_dev_t s_kb_dev = {
    .row_count = 5,
    .col_count = 3,
};

/* 字体对象（ASCII 8x16，与 test_display.c 同源，验证字库复用） */
static font_mgr_t s_font_8x16 = {
    .encoding  = FONT_MGR_ENCODING_ASCII,
    .width     = 8,
    .height    = 16,
    .base_addr = FONTS_ASCII_8X16_ADDR,
    .read      = dev_font_read,
};

/* 字体对象（GB2312 中文 16x16，含 ASCII 回退）—— 验证 TFT 也能渲染彩色中文 */
static font_mgr_t s_font_chs_16x16 = {
    .encoding  = FONT_MGR_ENCODING_GB2312,
    .width     = 16,
    .height    = 16,
    .base_addr = FONTS_CHS_16X16_ADDR,
    .read      = dev_font_read,
};

/* 中文串（GBK 原始字节）。源文件为 UTF-8，故用显式字节表达，与字库字节序一一对应，
 * 不依赖编译器源码编码：
 *   "彩色显示" = B2 CA C9 AB CF D4 CA BE
 *   "测试"     = B2 E2 CA D4
 */
static const uint8_t s_str_color[]  = {0xB2,0xCA,0xC9,0xAB,0xCF,0xD4,0xCA,0xBE,0x00};
static const uint8_t s_str_test[]   = {0xB2,0xE2,0xCA,0xD4,0x00};

/**
 * 画文字（PC 专属，验证 RGB565 字体渲染 + 前景/背景色）
 * 三行：英文黄字 + 彩色中文（青/绿），证明 TFT 能渲染彩色中文，而非仅色块。
 * 布局（240×240，避开顶部色带 y=0..59、底部色带 y=180..239）：
 *   y=72..87   英文 "TFT 240x240"（ASCII 8x16，黄字黑底）
 *   y=104..119 中文 "彩色显示"（GB2312 16x16，青字黑底）
 *   y=136..151 中文 "测试"（GB2312 16x16，绿字黑底）
 */
static void sim_draw_text(void)
{
    /* 黑底条带：y=68..163，统一背景，让文字清晰且不与色块串色 */
    svc_display_tft_area_t band = {0, 68, DEV_LCD_TFT_WIDTH, 96};
    svc_display_tft_area_fill_all(&band, DEV_LCD_TFT_BLACK);

    /* 英文：黄字 */
    svc_display_tft_rect_t r_en = {{0, 72, DEV_LCD_TFT_WIDTH, 16}, 0, 0};
    svc_display_tft_rect_text(&r_en,
                              (const uint8_t *)"TFT 240x240",
                              &s_font_8x16,
                              SVC_DISPLAY_TFT_ALIGN_CENTER,
                              DEV_LCD_TFT_YELLOW,
                              DEV_LCD_TFT_BLACK, 1);

    /* 中文「彩色显示」：青字 */
    svc_display_tft_rect_t r_c1 = {{0, 104, DEV_LCD_TFT_WIDTH, 16}, 0, 0};
    svc_display_tft_rect_text(&r_c1,
                              s_str_color,
                              &s_font_chs_16x16,
                              SVC_DISPLAY_TFT_ALIGN_CENTER,
                              DEV_LCD_TFT_CYAN,
                              DEV_LCD_TFT_BLACK, 1);

    /* 中文「测试」：绿字 */
    svc_display_tft_rect_t r_c2 = {{0, 136, DEV_LCD_TFT_WIDTH, 16}, 0, 0};
    svc_display_tft_rect_text(&r_c2,
                              s_str_test,
                              &s_font_chs_16x16,
                              SVC_DISPLAY_TFT_ALIGN_CENTER,
                              DEV_LCD_TFT_GREEN,
                              DEV_LCD_TFT_BLACK, 1);
}

/**
 * 把帧缓冲 dump 成 PPM 图像文件（PC 调试用，客观验证画面）
 * 用 dev_lcd_tft_get_pixel 逐像素读 RGB565 → 转 8bit RGB → 写 PPM。
 * 生成的 tft_dump.ppm 可用图片查看器/转换工具查看。
 */
static void sim_dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        gen_log_err("dump: cannot open %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", DEV_LCD_TFT_WIDTH, DEV_LCD_TFT_HEIGHT);
    for (uint16_t y = 0; y < DEV_LCD_TFT_HEIGHT; y++) {
        for (uint16_t x = 0; x < DEV_LCD_TFT_WIDTH; x++) {
            uint16_t c = dev_lcd_tft_get_pixel(&g_lcd_tft_dev, x, y);
            uint8_t rgb[3];
            rgb[0] = (uint8_t)((c >> 11) & 0x1F) << 3;   /* R 5→8 */
            rgb[1] = (uint8_t)((c >> 5)  & 0x3F) << 2;   /* G 6→8 */
            rgb[2] = (uint8_t)(c         & 0x1F) << 3;   /* B 5→8 */
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    gen_log_info("dump: wrote %s (%dx%d)\n", path, DEV_LCD_TFT_WIDTH, DEV_LCD_TFT_HEIGHT);
}

/**
 * sim_draw_live — 实时帧：随帧号变化的动态内容 + 按键反馈
 *
 * 演示窗口不是死图，但**不覆盖静态文字区（y=68..163）**：
 *   - 右下角帧计数 F#### 实时跳动（y=164..177，处于文字带与底部色块之间）
 *   - 按数字键 1-9 改变帧计数文字颜色（验证 键盘→dev_keyboard→显示 闭环）
 *
 * 每帧只改帧计数所在的小矩形（自绘黑底再画字），静态画面其余部分不动。
 */
static void sim_draw_live(uint32_t frame, uint16_t *text_color)
{
    /* 读键盘：数字键 1-9 选颜色（sim_keyboard 矩阵 row1..row3, col0..col2） */
    uint8_t raw[5 * 3];
    dev_keyboard_read(&s_kb_dev, raw);
    static const uint16_t palette[9] = {
        DEV_LCD_TFT_RED, DEV_LCD_TFT_GREEN, DEV_LCD_TFT_BLUE,
        DEV_LCD_TFT_YELLOW, DEV_LCD_TFT_CYAN, DEV_LCD_TFT_MAGENTA,
        DEV_LCD_TFT_GRAY, DEV_LCD_TFT_WHITE, DEV_LCD_TFT_BLACK,
    };
    /* 数字键 1-9 连续排布在矩阵 [1..3][0..2]，按下任一即换色 */
    for (int i = 0; i < 9; i++) {
        uint8_t r = 1 + i / 3, c = i % 3;
        if (raw[r * 3 + c]) { *text_color = palette[i]; break; }
    }

    /* 帧计数文字（黑底，颜色随按键变），右下角，证明帧在动 + 键盘闭环 */
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "F%-4u", frame % 10000);
        svc_display_tft_rect_t rect = {{DEV_LCD_TFT_WIDTH - 66, 164, 60, 14}, 0, 0};
        svc_display_tft_rect_text(&rect, (const uint8_t *)buf, &s_font_8x16,
                                  SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  *text_color, DEV_LCD_TFT_BLACK, 1);
    }
}

int main(void)
{
    int r;

    /* 初始化 TFT（走 SDL 后端，建 240×240 窗口） */
    r = dev_lcd_tft_init(&g_lcd_tft_dev);
    if (r != 0) {
        gen_log_err("TFT init FAIL (%d)\n", r);
        return 1;
    }
    gen_log_info("TFT init OK\n");

    /* 绘图层绑定（顺带初始化字库：PC 链接 sim_font.c） */
    r = svc_display_tft_init(&g_lcd_tft_dev);
    if (r != 0) {
        gen_log_err("TFT svc init FAIL (%d)\n", r);
        return 1;
    }
    gen_log_info("TFT svc init OK\n");

    /* 键盘初始化（PC：sim_keyboard.c 注入 SDL 键事件） */
    dev_keyboard_init(&s_kb_dev);

    /* 画自检画面（设备无关，与真屏共用） */
    test_tft_draw_pattern();
    /* 额外：PC 上测文字（真屏 demo 暂不测） */
    sim_draw_text();
    dev_lcd_tft_refresh(&g_lcd_tft_dev);
    gen_log_info("TFT test pattern drawn\n");

    /* dump 帧缓冲到 PPM（客观验证画面，无需人看 SDL 窗口） */
    sim_dump_ppm("tft_dump.ppm");

    gen_log_info("TFT live loop started\n");
    gen_log_info("  数字键 1-9 改帧计数颜色 | 点窗口 X 退出\n");

    /* ===== 实时主循环：周期重绘动态内容 + 按键反馈 =====
     * 单线程 pump 模型：主线程每帧画图 → refresh（写入 shadow_buf）→
     * sim_lcd_tft_pump()（同一线程 poll 事件 + 把 shadow_buf 刷上屏）。
     * 窗口/事件/渲染同线程，点 X 的 WM_CLOSE 才能正常送达 → 干净退出。
     */
    uint32_t frame = 0;
    uint16_t text_color = DEV_LCD_TFT_YELLOW;
    while (!sim_lcd_tft_should_quit()) {
        sim_draw_live(frame, &text_color);
        dev_lcd_tft_refresh(&g_lcd_tft_dev);
        sim_lcd_tft_pump();
        SDL_Delay(33);   /* ~30fps 应用帧率 */
        frame++;
    }

    /* 干净退出：销毁 SDL 资源（单线程，无需 join） */
    sim_lcd_tft_shutdown();
    gen_log_info("TFT sim exited cleanly\n");
    return 0;
}
