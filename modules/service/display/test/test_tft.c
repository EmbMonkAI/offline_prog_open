/**
 * @file test_tft.c
 * @brief TFT 自检画面绘制（设备无关，真屏/PC 共用）
 *
 * 只包含 test_tft_draw_pattern() —— 用 svc_display_tft 接口画一张自检画面，
 * 不依赖任何硬件（HAL/SPI/ST7789）。真屏装配在 app_main.c（tft_setup），
 * PC 入口见 test_tft_sim.c。
 *
 * 自检画面布局（按 DEV_LCD_TFT_WIDTH/HEIGHT 自适应，240×240 为例）：
 *   顶部 红/绿/蓝 三色块（验证颜色 + RGB 顺序）
 *   中部 白底（验证颜色饱和，真屏可叠文字）
 *   底部 黄/青/品红 三色块
 *   最外圈白边框 2px（验证 ST7789 地址偏移 X/Y_OFFSET 是否正确）
 *
 * 判断方法（详见 app_main.c / st7789_tft.c 注释）：
 *   颜色反相 → INVON/INVOFF；边框偏移 → X/Y_OFFSET；
 *   红蓝互换 → MADCTL BGR 位；镜像 → MADCTL MH/MV 位。
 */

#include "test_tft.h"
#include "svc_display_tft.h"

void test_tft_draw_pattern(void)
{
    svc_display_tft_area_t area = {0};
    uint16_t w = DEV_LCD_TFT_WIDTH;
    uint16_t h = DEV_LCD_TFT_HEIGHT;
    uint16_t band_h = h / 4;          /* 上/下色带各占 1/4 屏高（240 屏 = 60px） */
    uint16_t col_w = w / 3;           /* 三等分列宽 */

    /* 1. 顶部三色块：红/绿/蓝 */
    area.x = 0;               area.y = 0;          area.width = (uint16_t)(col_w + 1); area.height = band_h;
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_RED);
    area.x = (uint16_t)(col_w + 1);                area.width = col_w;
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_GREEN);
    area.x = (uint16_t)(col_w * 2 + 1);            area.width = (uint16_t)(w - col_w * 2 - 1);
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_BLUE);

    /* 2. 中部白底（真屏上可叠文字；PC 上 test_tft_sim 另画文字） */
    area.x = 0;               area.y = band_h;     area.width = w;
    area.height = (uint16_t)(h - band_h * 2);
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_WHITE);

    /* 3. 底部三色块：黄/青/品红 */
    area.x = 0;               area.y = (uint16_t)(h - band_h); area.width = (uint16_t)(col_w + 1); area.height = band_h;
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_YELLOW);
    area.x = (uint16_t)(col_w + 1);                area.width = col_w;
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_CYAN);
    area.x = (uint16_t)(col_w * 2 + 1);            area.width = (uint16_t)(w - col_w * 2 - 1);
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_MAGENTA);

    /* 4. 最外圈白边框：2 像素宽，用于判断偏移是否正确 */
    {
        svc_display_tft_line_t ln = {0};
        ln.point.x = 0; ln.point.y = 0;   ln.len = w;
        svc_display_tft_x_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.y = 1; svc_display_tft_x_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.y = (uint16_t)(h - 1); svc_display_tft_x_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.y = (uint16_t)(h - 2); svc_display_tft_x_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.x = 0; ln.point.y = 0;   ln.len = h;
        svc_display_tft_y_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.x = 1; svc_display_tft_y_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.x = (uint16_t)(w - 1); svc_display_tft_y_line(&ln, DEV_LCD_TFT_WHITE);
        ln.point.x = (uint16_t)(w - 2); svc_display_tft_y_line(&ln, DEV_LCD_TFT_WHITE);
    }
}
