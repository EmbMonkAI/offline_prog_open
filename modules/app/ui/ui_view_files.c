/**
 * @file ui_view_files.c
 * @brief 文件列表页（P1）绘制实现
 *
 * 布局见 ui_layout.h「文件列表页」：标题栏（青底黑字「SD卡文件（N个）」）
 * + 列表项（选中项反白高亮 + 「▸」标记，非选中黑底白字）+ 底部按键提示。
 * 列表超出可见区（UI_FILES_VISIBLE）时按选中项滚动（选中项保持可见）。
 *
 * 中文串用显式 GBK 字节（源文件 UTF-8，字库读 GBK，见 DEVELOPMENT.md §13.8）。
 */

#include "ui_layout.h"
#define GEN_LOG_MODULE  1
#define GEN_LOG_TAG     "ui-files"
#include "ui_view_files.h"
#include "svc_display_tft.h"
#include "font_mgr.h"
#include "gen_log.h"
#include <stdio.h>
#include <string.h>

/* ==================== 中文字节常量（GBK）==================== */
/* 标题「SD卡文件 N个」为混合串（ASCII+中文），逐段渲染见 draw 内（回退区缺字形，§见下） */
static const uint8_t S_NONE[]   = {0xCE,0xDE,0xCE,0xC4,0xBC,0xFE,0x00};                        /* 无文件 */
static const uint8_t S_HINT_UP[]= {0xD1,0xA1,0xD4,0xF1,0x00};                                  /* 选择 */
static const uint8_t S_HINT_OK[]= {0xC8,0xB7,0xC8,0xCF,0x00};                                  /* 确认 */
static const uint8_t S_ARROW[]  = {0xA1,0xFA,0x00};                                            /* → (GBK A1 FA) */

static font_mgr_t *s_ascii;
static font_mgr_t *s_chs;

void ui_view_files_init(font_mgr_t *ascii_font, font_mgr_t *chs_font)
{
    s_ascii = ascii_font;
    s_chs   = chs_font;
}

void ui_view_files_clear(void)
{
    svc_display_tft_clear(DEV_LCD_TFT_BLACK);
}

void ui_view_files_draw(const uint8_t *const files[], uint16_t count, uint16_t selected)
{
    /* ---- 标题栏：整条青底，黑字「SD卡文件 N 个」（左右留 4px 内边距，文字垂直居中）----
     * ⚠️ 混合串不能整串走 s_chs：16x16 字库的 GB2312→ASCII 回退区数据不全
     * （'S''D' 等字母渲染为空，实测 2026-08-28）→ 中文段走 s_chs、ASCII 段走 s_ascii
     * 逐段续接渲染（与真机 app_ui 的值字体自动选同理）。 */
    svc_display_tft_area_t bar = {0, UI_FILES_TITLE_Y, UI_LCD_W, UI_FILES_TITLE_H};
    svc_display_tft_area_fill_all(&bar, DEV_LCD_TFT_CYAN);
    {
        uint8_t cnt[8];
        static const uint8_t GE[] = {0xB8,0xF6,0x00};       /* 个 */
        static const uint8_t WENJIAN[] = {0xBF,0xA8,0xCE,0xC4,0xBC,0xFE,0x00};  /* 卡文件 */
        uint16_t x = 4;
        uint16_t ty = (uint16_t)(UI_FILES_TITLE_Y + (UI_FILES_TITLE_H - UI_FONT_H) / 2);
        snprintf(cnt, sizeof(cnt), "%u", (unsigned)count);

        /* 逐段：'SD'(ASCII) → '卡文件'(中文) → ' N '(ASCII) → '个'(中文) */
        svc_display_tft_rect_t r = {0, ty, 0, UI_FONT_H};
        r.edge_width = 0; r.edge_options = SVC_DISPLAY_TFT_EDGE_NO;

        r.area.x = x; r.area.width = 2 * (UI_FONT_W_ASCII + 1);
        svc_display_tft_rect_text(&r, (const uint8_t *)"SD", s_ascii, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
        x += (uint16_t)(font_mgr_get_width(s_ascii, (const uint8_t *)"SD", 1) + 2);

        r.area.x = x; r.area.width = (uint16_t)(3 * UI_FONT_W_CHS);
        svc_display_tft_rect_text(&r, WENJIAN, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
        x += (uint16_t)(font_mgr_get_width(s_chs, WENJIAN, 1) + 3);

        r.area.x = x; r.area.width = (uint16_t)(strlen((char *)cnt) * (UI_FONT_W_ASCII + 1));
        svc_display_tft_rect_text(&r, cnt, s_ascii, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
        x += (uint16_t)(font_mgr_get_width(s_ascii, cnt, 1) + 3);

        r.area.x = x; r.area.width = UI_FONT_W_CHS;
        svc_display_tft_rect_text(&r, GE, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                  DEV_LCD_TFT_BLACK, DEV_LCD_TFT_CYAN, 1);
        (void)x;
    }

    /* ---- 列表 ---- */
    if (count == 0) {
        svc_display_tft_rect_t r = {{0, UI_FILES_LIST_Y + 8, UI_LCD_W, UI_FONT_H}, 0, 0};
        svc_display_tft_rect_text(&r, S_NONE, s_chs, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_GRAY, DEV_LCD_TFT_BLACK, 1);
        return;
    }
    if (selected >= count) selected = 0;

    /* 滚动窗口：选中项保持可见 */
    uint16_t top = 0;
    if (selected >= UI_FILES_VISIBLE)
        top = (uint16_t)(selected - UI_FILES_VISIBLE + 1);
    if (top + UI_FILES_VISIBLE > count && count >= UI_FILES_VISIBLE)
        top = (uint16_t)(count - UI_FILES_VISIBLE);

    uint16_t shown = (count < UI_FILES_VISIBLE) ? count : UI_FILES_VISIBLE;
    for (uint16_t i = 0; i < shown; i++) {
        uint16_t idx = (uint16_t)(top + i);
        uint16_t y = (uint16_t)(UI_FILES_LIST_Y + i * UI_FILES_ITEM_H);
        int is_sel = (idx == selected);

        /* 选中项整行反白底（白底黑字），非选中黑底白字 */
        svc_display_tft_area_t row = {0, y, UI_LCD_W, UI_FILES_ITEM_H};
        svc_display_tft_area_fill_all(&row, is_sel ? DEV_LCD_TFT_WHITE : DEV_LCD_TFT_BLACK);

        if (is_sel) {
            /* 「→」选中标记（青色，置于行首，行内垂直居中） */
            svc_display_tft_rect_t mk = {{2, (uint16_t)(y + (UI_FILES_ITEM_H - UI_FONT_H) / 2),
                                          UI_FILES_MARK_W, UI_FONT_H}, 0, 0};
            svc_display_tft_rect_text(&mk, S_ARROW, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                      DEV_LCD_TFT_RED, DEV_LCD_TFT_WHITE, 1);
        }

        /* 文件名：截断到可视宽（标记宽 + 右缘留 8px；字宽 9px 含 1px 间距）*/
        {
            char name[40];
            const char *src = (const char *)files[idx];
            uint16_t avail_w = (uint16_t)(UI_LCD_W - UI_FILES_ITEM_TEXT_X - UI_FILES_MARK_W - 8);
            uint16_t max_chars = (uint16_t)(avail_w / (UI_FONT_W_ASCII + 1));   /* 8px 字 + 1px 间距 */
            snprintf(name, sizeof(name), "%s", src);
            if (strlen(name) > max_chars && max_chars >= 3) {
                name[max_chars - 3] = 0;
                strcat(name, "...");
            }
            svc_display_tft_rect_t r = {{(uint16_t)(UI_FILES_ITEM_TEXT_X + UI_FILES_MARK_W),
                                         (uint16_t)(y + (UI_FILES_ITEM_H - UI_FONT_H) / 2),
                                         avail_w,
                                         UI_FONT_H}, 0, 0};
            svc_display_tft_rect_text(&r, (const uint8_t *)name, s_ascii, SVC_DISPLAY_TFT_ALIGN_LEFT,
                                      is_sel ? DEV_LCD_TFT_BLACK : DEV_LCD_TFT_WHITE,
                                      is_sel ? DEV_LCD_TFT_WHITE : DEV_LCD_TFT_BLACK, 1);
        }
    }

    /* ---- 底部提示：「选择 / 确认」（灰字，提示区内垂直居中）---- */
    {
        svc_display_tft_rect_t r = {{4, (uint16_t)(UI_FILES_HINT_Y + (UI_FILES_HINT_H - UI_FONT_H) / 2), UI_LCD_W - 8, UI_FONT_H}, 0, 0};
        uint8_t hint[32];
        uint16_t p = 0;
        memcpy(hint + p, S_HINT_UP, sizeof(S_HINT_UP) - 1); p += sizeof(S_HINT_UP) - 1;
        hint[p++] = ' '; hint[p++] = '/'; hint[p++] = ' ';
        memcpy(hint + p, S_HINT_OK, sizeof(S_HINT_OK) - 1); p += sizeof(S_HINT_OK) - 1;
        hint[p] = 0;
        svc_display_tft_rect_text(&r, hint, s_chs, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_GRAY, DEV_LCD_TFT_BLACK, 1);
    }
}
