/**
 * @file test_tft_quitprobe.c
 * @brief 验证 TFT 模拟「点关闭 → 干净退出」路径（自动测试，免人工点窗口）
 *
 * 复用 test_tft 的全部逻辑（init/自检/实时主循环），唯一区别：
 * 起一个辅助线程，1.5s 后向 SDL 队列 push 一个 SDL_QUIT 事件 ——
 * 等价于用户点窗口 X。若退出路径干净，main 返回 0；旧代码（exit(0) 在
 * 渲染线程内）会弹错误对话框 / 返回非 0。
 *
 * 用途：CI/脚本里自动验证 close 不崩。正常手测用 test_tft。
 */

#include "dev_lcd_tft.h"
#define GEN_LOG_TAG     "tft-quitprobe"
#define GEN_LOG_MODULE 1
#include "gen_log.h"
#include "svc_display_tft.h"
#include "font_mgr.h"
#include "dev_font.h"
#include "kp_font_lib.h"
#include "dev_keyboard.h"
#include "test_tft.h"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>
#include <pthread.h>

extern dev_lcd_tft_dev_t g_lcd_tft_dev;
extern int  sim_lcd_tft_should_quit(void);
extern void sim_lcd_tft_shutdown(void);
extern void sim_lcd_tft_pump(void);

static dev_keyboard_dev_t s_kb_dev = { .row_count = 5, .col_count = 3 };

static font_mgr_t s_font_8x16 = {
    .encoding  = FONT_MGR_ENCODING_ASCII,
    .width     = 8, .height = 16,
    .base_addr = FONTS_ASCII_8X16_ADDR,
    .read      = dev_font_read,
};

/* 辅助线程：1.5s 后向 SDL 窗口发真正的 WM_CLOSE —— 与用户点窗口 X 走完全
 * 相同的路径（Win32 消息泵，不绕过）。用它能抓到「事件/窗口跨线程」的死锁；
 * 若改用 SDL_PushEvent(SDL_QUIT) 会绕过消息泵、漏掉该 bug。 */
#ifdef _WIN32
#include <windows.h>
#include <SDL2/SDL_syswm.h>
extern SDL_Window *sim_lcd_tft_get_window(void);   /* sim_lcd_tft.c 暴露 */
static void *inject_quit(void *arg)
{
    (void)arg;
    SDL_Delay(1500);
    SDL_Window *win = sim_lcd_tft_get_window();
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (win && SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        HWND hwnd = info.info.win.window;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);   /* 等价点 X */
        gen_log_info("posted WM_CLOSE to HWND (real close-button path)\n");
    } else {
        gen_log_err("cannot get HWND, fallback to SDL_PushEvent\n");
        SDL_Event e = { .type = SDL_QUIT };
        SDL_PushEvent(&e);
    }
    return NULL;
}
#else
static void *inject_quit(void *arg)
{
    (void)arg;
    SDL_Delay(1500);
    SDL_Event e = { .type = SDL_QUIT };
    SDL_PushEvent(&e);
    gen_log_info("injected SDL_QUIT (non-Windows)\n");
    return NULL;
}
#endif

int main(void)
{
    int r;
    r = dev_lcd_tft_init(&g_lcd_tft_dev);
    if (r) { gen_log_err("init FAIL\n"); return 1; }
    r = svc_display_tft_init(&g_lcd_tft_dev);
    if (r) { gen_log_err("svc FAIL\n"); return 1; }
    dev_keyboard_init(&s_kb_dev);

    /* 起注入线程 */
    pthread_t th;
    pthread_create(&th, NULL, inject_quit, NULL);

    test_tft_draw_pattern();
    dev_lcd_tft_refresh(&g_lcd_tft_dev);

    /* 简化主循环（不画动态，只验证退出路径） */
    uint32_t frame = 0;
    uint16_t bar_color = DEV_LCD_TFT_RED, bar_x = 4;
    while (!sim_lcd_tft_should_quit()) {
        /* 复用最简动态：移动竖条（用 inline，避免依赖 test_tft_sim 的 static 函数） */
        svc_display_tft_area_t canvas = {0, 30, DEV_LCD_TFT_WIDTH, 40};
        svc_display_tft_area_fill_all(&canvas, DEV_LCD_TFT_WHITE);
        bar_x = (uint16_t)(4 + (frame % 116));
        for (uint16_t dx = 0; dx < 4; dx++) {
            svc_display_tft_line_t ln = {{(uint16_t)(bar_x + dx), 34}, 30};
            svc_display_tft_y_line(&ln, bar_color);
        }
        (void)s_font_8x16;
        dev_lcd_tft_refresh(&g_lcd_tft_dev);
        sim_lcd_tft_pump();
        SDL_Delay(33);
        frame++;
    }

    pthread_join(th, NULL);
    sim_lcd_tft_shutdown();
    gen_log_info("QUIT-PROBE: exited cleanly with code 0\n");
    return 0;
}
