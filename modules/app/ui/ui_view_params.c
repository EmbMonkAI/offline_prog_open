/**
 * @file ui_view_params.c
 * @brief 参数页（P2）绘制实现
 *
 * 布局见 ui_layout.h「参数页」：标题栏（青底「文件参数」）+ 文件名行（黄字滚动）
 * + 分隔线 + 六行「标签: 值」（标签青、值白：芯片/固件/地址/大小/保护/次数）
 * + 底部动作区（绿底白字「开始烧录」，OK 键触发）。
 *
 * 中文串用显式 GBK 字节（见 DEVELOPMENT.md §13.8）。
 */

#include "ui_layout.h"
#define GEN_LOG_MODULE  1
#define GEN_LOG_TAG     "ui-params"
#include "ui_view_params.h"
#include "svc_display_tft.h"
#include "font_mgr.h"
#include "gen_log.h"
#include <stdio.h>
#include <string.h>

/* ==================== 中文字节常量（GBK）==================== */
static const uint8_t S_TITLE[] = {0xCE,0xC4,0xBC,0xFE,0xB2,0xCE,0xCA,0xFD,0x00};   /* 文件参数 */
static const uint8_t S_CHIP[]  = {0xD0,0xBE,0xC6,0xAC,0x00};                        /* 芯片 */
static const uint8_t S_FW[]    = {0xB9,0xCC,0xBC,0xFE,0x00};                        /* 固件 */
static const uint8_t S_ADDR[]  = {0xB5,0xD8,0xD6,0xB7,0x00};                        /* 地址 */
static const uint8_t S_SIZE[]  = {0xB4,0xF3,0xD0,0xA1,0x00};                        /* 大小 */
static const uint8_t S_PROT[]  = {0xB1,0xA3,0xBB,0xA4,0x00};                        /* 保护 */
static const uint8_t S_ON[]    = {0xBF,0xAA,0x00};                                  /* 开 */
static const uint8_t S_OFF[]   = {0xB9,0xD8,0x00};                                  /* 关 */
static const uint8_t S_COUNT[] = {0xB4,0xCE,0xCA,0xFD,0x00};                        /* 次数 */
static const uint8_t S_GO[]    = {0xBF,0xAA,0xCA,0xBC,0xC9,0xD5,0xC2,0xBC,0x00};    /* 开始烧录 */

static font_mgr_t *s_ascii;
static font_mgr_t *s_chs;

/* 文件名滚动相位（与 ui_view_program 同款机制） */
static uint8_t s_scroll_px;
static uint8_t s_scroll_div;
#define SCROLL_TICK_DIV 6

/* ==================== 内部辅助 ==================== */

/** 画一行「标签: 值」：标签青（中文），值白（值字体按内容自动选：首字节>=0x80
 *  （GBK 中文）用中文字体，纯 ASCII 用 ASCII 字体 —— 与真机 app_ui draw_burn_row 一致，
 *  修「保护: 开/关」中文值乱码） */
static void draw_row(uint16_t y, const uint8_t *label, const char *value)
{
    svc_display_tft_rect_t r;
    static const uint8_t COLON[] = {':', 0x00};
    font_mgr_t *vfont = (value && (uint8_t)value[0] >= 0x80) ? s_chs : s_ascii;

    r.area.x = UI_PARAMS_LABEL_X; r.area.y = y;
    r.area.width = UI_PARAMS_LABEL_W; r.area.height = UI_PARAMS_ROW_H;
    r.edge_width = 0; r.edge_options = SVC_DISPLAY_TFT_EDGE_NO;
    svc_display_tft_rect_text(&r, label, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_CYAN, DEV_LCD_TFT_BLACK, 1);

    uint16_t lx = (uint16_t)(UI_PARAMS_LABEL_X + (uint16_t)font_mgr_get_width(s_chs, label, 1));
    r.area.x = lx; r.area.width = (uint16_t)(UI_PARAMS_LABEL_X + UI_PARAMS_LABEL_W - lx);
    svc_display_tft_rect_text(&r, COLON, s_ascii, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_CYAN, DEV_LCD_TFT_BLACK, 1);

    r.area.x = UI_PARAMS_VALUE_X; r.area.width = UI_PARAMS_VALUE_W;
    svc_display_tft_rect_text(&r, (const uint8_t *)value, vfont, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_WHITE, DEV_LCD_TFT_BLACK, 1);
}

/** 字节数人性化：512KB / 1MB / 13692 B */
static void fmt_size(uint32_t b, char *out, size_t out_sz)
{
    if (b >= 1024u * 1024u && (b % (1024u * 1024u)) == 0)
        snprintf(out, out_sz, "%lu MB", (unsigned long)(b / (1024u * 1024u)));
    else if (b >= 1024u && (b % 1024u) == 0)
        snprintf(out, out_sz, "%lu KB", (unsigned long)(b / 1024u));
    else
        snprintf(out, out_sz, "%lu B", (unsigned long)b);
}

/* ==================== 对外接口 ==================== */

void ui_view_params_init(font_mgr_t *ascii_font, font_mgr_t *chs_font)
{
    s_ascii = ascii_font;
    s_chs   = chs_font;
    s_scroll_px = 0;
    s_scroll_div = 0;
}

void ui_view_params_clear(void)
{
    svc_display_tft_clear(DEV_LCD_TFT_BLACK);
}

void ui_view_params_scroll_tick(const uint8_t *filename)
{
    if (!filename) return;
    uint32_t w = font_mgr_get_width(s_ascii, filename, 1);
    if (w <= UI_LCD_W) return;   /* 不超宽，不滚 */

    if (++s_scroll_div < SCROLL_TICK_DIV) return;
    s_scroll_div = 0;

    uint16_t max_px = (uint16_t)(w - UI_LCD_W) + 16;
    s_scroll_px++;
    if (s_scroll_px > max_px) s_scroll_px = 0;
}

void ui_view_params_draw(const ui_params_state_t *state)
{
    if (!state) return;

    /* ---- 标题栏：青底黑字「文件参数」---- */
    {
        svc_display_tft_area_t bar = {0, UI_PARAMS_TITLE_Y, UI_LCD_W, UI_PARAMS_TITLE_H};
        svc_display_tft_area_fill_all(&bar, DEV_LCD_TFT_CYAN);
        svc_display_tft_rect_t r = {{2, UI_PARAMS_TITLE_Y, UI_LCD_W - 4, UI_PARAMS_TITLE_H}, 0, 0};
        svc_display_tft_rect_text(&r, S_TITLE, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
    }

    /* ---- 文件名行（黄字，超宽滚动）---- */
    {
        uint32_t w = font_mgr_get_width(s_ascii, state->filename, 1);
        svc_display_tft_area_t row = {0, UI_PARAMS_FILENAME_Y, UI_LCD_W, UI_PARAMS_FILENAME_H};
        svc_display_tft_area_fill_all(&row, DEV_LCD_TFT_BLACK);

        if (w <= UI_LCD_W) {
            svc_display_tft_rect_t r = {{0, UI_PARAMS_FILENAME_Y, UI_LCD_W, UI_PARAMS_FILENAME_H}, 0, 0};
            svc_display_tft_rect_text(&r, state->filename, s_ascii, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                      DEV_LCD_TFT_YELLOW, DEV_LCD_TFT_BLACK, 1);
        } else {
            /* 逐字符 blit，按滚动相位偏移（与 ui_view_program.draw_filename 同法） */
            const uint8_t *p = state->filename;
            int32_t cur_x = -(int32_t)s_scroll_px;
            uint8_t fdata[128];
            while (*p) {
                uint32_t code = font_mgr_get_code(s_ascii, p);
                uint32_t cw = font_mgr_get_pixel_data(s_ascii, code, fdata);
                int32_t left = cur_x, right = cur_x + (int32_t)cw;
                if (right > 0 && left < (int32_t)UI_LCD_W) {
                    svc_display_tft_area_t ca = {(uint16_t)(left > 0 ? left : 0),
                                                 UI_PARAMS_FILENAME_Y,
                                                 (uint8_t)cw, UI_FONT_H};
                    svc_display_tft_area_pixel(&ca, fdata, DEV_LCD_TFT_YELLOW,
                                               (svc_display_tft_color_t)SVC_DISPLAY_TFT_TRANSPARENT);
                }
                cur_x += (int32_t)cw + 1;
                p = font_mgr_get_next(s_ascii, p);
                if (*p == 0) break;
            }
        }
    }

    /* ---- 分隔线 ---- */
    {
        svc_display_tft_line_t ln = {{0, UI_PARAMS_SEP_Y}, UI_LCD_W};
        svc_display_tft_x_line(&ln, DEV_LCD_TFT_GRAY);
    }

    /* ---- 六行参数 ---- */
    {
        char v[24];

        draw_row(UI_PARAMS_ROW1_Y, S_CHIP, (const char *)state->chip_model);

        fmt_size(state->fw_size, v, sizeof(v));
        draw_row(UI_PARAMS_ROW2_Y, S_FW, v);

        snprintf(v, sizeof(v), "0x%08lX", (unsigned long)state->flash_start);
        draw_row(UI_PARAMS_ROW3_Y, S_ADDR, v);

        fmt_size(state->flash_size, v, sizeof(v));
        draw_row(UI_PARAMS_ROW4_Y, S_SIZE, v);

        draw_row(UI_PARAMS_ROW5_Y, S_PROT, state->rdp ? (const char *)S_ON : (const char *)S_OFF);

        snprintf(v, sizeof(v), "%lu", (unsigned long)state->burn_count);
        draw_row(UI_PARAMS_ROW6_Y, S_COUNT, v);
    }

    /* ---- 底部动作区：绿底白字「开始烧录」（文字垂直居中）---- */
    {
        svc_display_tft_area_t act = {UI_PARAMS_ACTION_X, UI_PARAMS_ACTION_Y,
                                      UI_PARAMS_ACTION_W, UI_PARAMS_ACTION_H};
        svc_display_tft_area_fill_all(&act, DEV_LCD_TFT_GREEN);
        svc_display_tft_rect_t r = {{UI_PARAMS_ACTION_X, (uint16_t)(UI_PARAMS_ACTION_Y + (UI_PARAMS_ACTION_H - UI_FONT_H) / 2),
                                     UI_PARAMS_ACTION_W, UI_FONT_H}, 0, 0};
        svc_display_tft_rect_text(&r, S_GO, s_chs, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_WHITE, DEV_LCD_TFT_GREEN, 1);
    }
}
