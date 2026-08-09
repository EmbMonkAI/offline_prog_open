/**
 * @file ui_view_program.c
 * @brief 烧录界面（program view）绘制实现
 *
 * 纯绘制层：把 ui_prog_state_t 画到 TFT。要素：
 *   1. 文件名行 —— 超宽时按滚动相位水平滚动（ALIGN_LEFT，相位偏移 x）
 *   2. 三行参数 —— 芯片型号 / 次数+供电 / 保护+状态，标签青、值白
 *   3. 进度条 —— rect_fill_all 画外框+底，area_fill_all 画已填充段，百分比文字
 *   4. 结果区 —— 反白大字：成功=绿底白字，失败=红底白字（最醒目）
 *
 * 复用 svc_display_tft_* 与 font_mgr，不读任何硬件。
 *
 * 中文串全部用显式 GBK 字节（源文件 UTF-8，字库读 GBK，见 DEVELOPMENT.md §13.8）。
 */

#include "ui_layout.h"
#define GEN_LOG_MODULE  1
#define GEN_LOG_TAG     "ui-prog"
#include "ui_view_program.h"
#include "svc_display_tft.h"
#include "font_mgr.h"
#include "gen_log.h"
#include <stdio.h>
#include <string.h>

/* ==================== 中文字节常量（GBK）====================
 * 供电 USB/电池/外接、保护 开/关、状态 就绪/烧录中、结果 烧录成功/失败
 * 用 Python '...'.encode('gbk') 得到，避免源码编码依赖。
 */
static const uint8_t S_CHIP[]   = {0xD0,0xBE,0xC6,0xAC,0x00};                 /* 芯片 */
static const uint8_t S_COUNT[]  = {0xB4,0xCE,0xCA,0xFD,0x00};                 /* 次数 */
static const uint8_t S_PROT[]   = {0xB1,0xA3,0xBB,0xA4,0x00};                 /* 保护 */

static const uint8_t S_ON[]     = {0xBF,0xAA,0x00};                           /* 开 */
static const uint8_t S_OFF[]    = {0xB9,0xD8,0x00};                           /* 关 */

static const uint8_t S_BUSY[]   = {0xC9,0xD5,0xC2,0xBC,0xD6,0xD0,0x00};       /* 烧录中 */

static const uint8_t S_OK[]     = {0xC9,0xD5,0xC2,0xBC,0xB3,0xC9,0xB9,0xA6,0x00}; /* 烧录成功 */
static const uint8_t S_FAIL[]   = {0xC9,0xD5,0xC2,0xBC,0xCA,0xA7,0xB0,0xDC,0x00}; /* 烧录失败 */
static const uint8_t S_HINT[]   = {0xB0,0xB4,0xC8,0xB7,0xC8,0xCF,0xBF,0xAA,0xCA,0xBC,0xC9,0xD5,0xC2,0xBC,0x00}; /* 按确认开始烧录 */

/* ==================== 字体（init 时绑定）==================== */
static font_mgr_t *s_ascii;     /* ASCII 8×16 */
static font_mgr_t *s_chs;       /* GB2312 16×16（含 ASCII 回退） */

/* 文件名滚动相位（像素偏移），由 ui_view_program_scroll_tick 驱动 */
static uint8_t  s_scroll_px;
/* 滚动节奏：每 SCROLL_TICK_DIV 帧推进 1px */
#define SCROLL_TICK_DIV   6
static uint8_t  s_scroll_divider;

/* ==================== 内部辅助 ==================== */

/** 画一个键值对：「标签: 值」紧凑排列（标签后跟冒号，再画值）
 *   label_x : 标签起始 x
 *   max_x   : 该键值对允许的最大右边界（值不得越过此线，防侵入相邻列）
 *   label 用中文字体（s_chs）；冒号「:」用 ASCII 字体（青色，与标签同色）。
 *   value 字体按内容自动选：含中文（首字节>=0x80）用 s_chs，纯 ASCII 用 s_ascii
 *     （子集字库的 GB2312→ASCII 回退地址为空，ASCII 必须走 s_ascii，见 §13.8）。
 *   返回：值实际画到的右边界 x（供调试/后续排版用）。
 */
static uint16_t draw_kv(uint16_t y, const uint8_t *label, const uint8_t *value,
                        uint16_t label_x, uint16_t max_x)
{
    svc_display_tft_rect_t r;
    /* 自动选值字体：首字节>=0x80 视为中文串 */
    font_mgr_t *vfont = (value && value[0] >= 0x80) ? s_chs : s_ascii;
    static const uint8_t COLON[] = {':', 0x00};   /* 冒号（ASCII） */

    /* 标签（青色，中文），从 label_x 起 ALIGN_LEFT */
    r.area.x = label_x; r.area.y = y;
    r.area.width = max_x - label_x; r.area.height = UI_PROG_PARAM_H;
    r.edge_width = 0; r.edge_options = SVC_DISPLAY_TFT_EDGE_NO;
    svc_display_tft_rect_text(&r, label, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_CYAN, DEV_LCD_TFT_BLACK, 1);

    /* 冒号（青色，ASCII），紧跟标签后 */
    uint16_t label_w = (uint16_t)font_mgr_get_width(s_chs, label, 1);
    uint16_t cur_x = label_x + label_w;
    r.area.x = cur_x; r.area.width = max_x - cur_x;
    svc_display_tft_rect_text(&r, COLON, s_ascii, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_CYAN, DEV_LCD_TFT_BLACK, 1);

    /* 值（白色）：冒号后 + 间距 */
    uint16_t val_x = cur_x + (uint16_t)font_mgr_get_width(s_ascii, COLON, 1) + UI_PROG_KV_GAP;
    if (val_x > max_x) val_x = max_x;   /* 越界钳制，避免侵入相邻列 */
    r.area.x = val_x; r.area.width = max_x - val_x;
    svc_display_tft_rect_text(&r, value, vfont, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_WHITE, DEV_LCD_TFT_BLACK, 1);
    return val_x + (uint16_t)font_mgr_get_width(vfont, value, 1);
}

/** 画文件名行：超宽则按滚动相位偏移，画在一个固定宽度窗口里（多余被后续分隔线/参数覆盖） */
static void draw_filename(const uint8_t *filename)
{
    if (!filename)
        return;

    uint32_t w = font_mgr_get_width(s_ascii, filename, 1);
    uint16_t offset = 0;
    if (w > UI_PROG_FILENAME_W)
        offset = s_scroll_px;   /* 超宽才滚动 */

    /* 用 ALIGN_LEFT，x 从 -offset 起，但 rect_text 的 area.x 是 uint16 不能为负。
     * 解法：area.x 固定为文件名窗口起点，rect_text 内部按 ALIGN_LEFT 从 area.x 开始画；
     * 滚动通过「截取子串」实现太复杂，这里用更简单的窗口裁剪：
     * 文件名窗口固定在顶部，rect_text 用 ALIGN_LEFT 画整串，
     * 超出 UI_PROG_FILENAME_W 右侧的部分会被下一行（参数区/分隔线）覆盖，
     * 但中间几行不会被覆盖 —— 故必须显式裁剪。
     * 采用「整行先黑底，再在裁剪窗口内画」：见下方简化实现。 */
    svc_display_tft_rect_t r;
    r.area.x = UI_PROG_FILENAME_X; r.area.y = UI_PROG_FILENAME_Y;
    r.area.width = UI_PROG_FILENAME_W; r.area.height = UI_PROG_FILENAME_H;
    r.edge_width = 0; r.edge_options = SVC_DISPLAY_TFT_EDGE_NO;

    if (w <= UI_PROG_FILENAME_W) {
        /* 不超宽：直接居中/左对齐画整串 */
        svc_display_tft_rect_text(&r, filename, s_ascii, SVC_DISPLAY_TFT_ALIGN_CENTER,
                                  DEV_LCD_TFT_YELLOW, DEV_LCD_TFT_BLACK, 1);
        return;
    }

    /* 超宽滚动：手动按 offset 取可见窗口内的字符子段画。
     * rect_text 不支持负 x / 水平裁剪，故这里逐字符画并裁剪到窗口宽。 */
    (void)offset;
    /* 先黑底 */
    svc_display_tft_area_t area = {UI_PROG_FILENAME_X, UI_PROG_FILENAME_Y,
                                   UI_PROG_FILENAME_W, UI_PROG_FILENAME_H};
    svc_display_tft_area_fill_all(&area, DEV_LCD_TFT_BLACK);

    /* 逐字符 blit，累计像素宽，跳过 offset 之前、截断窗口之后 */
    const uint8_t *p = filename;
    int32_t cur_x = UI_PROG_FILENAME_X - (int32_t)s_scroll_px; /* 字符左边绝对 x（可能为负） */
    uint8_t fdata[128];
    while (*p) {
        uint32_t code = font_mgr_get_code(s_ascii, p);
        uint32_t cw = font_mgr_get_pixel_data(s_ascii, code, fdata);
        int32_t char_left = cur_x;
        int32_t char_right = cur_x + (int32_t)cw;
        /* 字符与可见窗口 [X, X+W) 相交才画 */
        if (char_right > UI_PROG_FILENAME_X && char_left < UI_PROG_FILENAME_X + UI_PROG_FILENAME_W) {
            svc_display_tft_area_t ca;
            ca.x = (uint16_t)char_left;
            ca.y = UI_PROG_FILENAME_Y;
            ca.width = (uint8_t)cw;
            ca.height = UI_FONT_H;
            /* area_pixel 不做水平裁剪，char_left<0 时会越界返回；用透明背景，
             * 让窗口外部分自然被黑底/相邻行覆盖。仅画在窗口内的靠 dev_lcd_tft_set_pixel 越界保护 */
            svc_display_tft_area_pixel(&ca, fdata, DEV_LCD_TFT_YELLOW,
                                       (svc_display_tft_color_t)SVC_DISPLAY_TFT_TRANSPARENT);
        }
        cur_x += (int32_t)cw + 1;
        p = font_mgr_get_next(s_ascii, p);
        if (*p == 0) break;
    }
}

/** 画烧录中区域：状态文字「烧录中...」+ 进度条（整行宽，无百分比）
 *   仅在「进行中」(0<progress<100) 调用。完成后由结果区取代，不画进度条。 */
static void draw_burning(uint8_t progress)
{
    if (progress > 100) progress = 100;

    /* 状态文字「烧录中...」（青色，进度条上方一行） */
    svc_display_tft_rect_t sr;
    sr.area.x = 2; sr.area.y = UI_PROG_STATUS_Y;
    sr.area.width = UI_LCD_W - 4; sr.area.height = UI_PROG_STATUS_H;
    sr.edge_width = 0; sr.edge_options = SVC_DISPLAY_TFT_EDGE_NO;
    svc_display_tft_rect_text(&sr, S_BUSY, s_chs, SVC_DISPLAY_TFT_ALIGN_LEFT,
                              DEV_LCD_TFT_CYAN, DEV_LCD_TFT_BLACK, 1);

    /* 进度条外框（亮边框 + 黑底），整行宽 */
    svc_display_tft_rect_t r;
    r.area.x = UI_PROG_BAR_X; r.area.y = UI_PROG_BAR_Y;
    r.area.width = UI_PROG_BAR_W; r.area.height = UI_PROG_BAR_H;
    r.edge_width = UI_PROG_BAR_EDGE;
    r.edge_options = SVC_DISPLAY_TFT_EDGE_ALL;
    svc_display_tft_rect_fill_all(&r, DEV_LCD_TFT_WHITE, DEV_LCD_TFT_BLACK);

    /* 已填充段：宽度按比例（边框内），进行中青色 */
    uint16_t inner_w = UI_PROG_BAR_W - 2 * UI_PROG_BAR_EDGE;
    uint16_t fill_w = (uint16_t)((uint32_t)inner_w * progress / 100);
    if (fill_w) {
        svc_display_tft_area_t fa;
        fa.x = UI_PROG_BAR_X + UI_PROG_BAR_EDGE;
        fa.y = UI_PROG_BAR_Y + UI_PROG_BAR_EDGE;
        fa.width = fill_w;
        fa.height = UI_PROG_BAR_H - 2 * UI_PROG_BAR_EDGE;
        svc_display_tft_area_fill_all(&fa, DEV_LCD_TFT_CYAN);
    }
}

/** 画结果区：反白大字（成功绿底/失败红底/无结果提示） */
static void draw_result(ui_prog_result_t result)
{
    svc_display_tft_color_t bg;
    const uint8_t *txt;
    uint8_t show_big = 0;

    switch (result) {
    case UI_PROG_RESULT_OK:
        bg = DEV_LCD_TFT_GREEN; txt = S_OK;   show_big = 1; break;
    case UI_PROG_RESULT_FAIL:
        bg = DEV_LCD_TFT_RED;   txt = S_FAIL; show_big = 1; break;
    default:
        bg = DEV_LCD_TFT_BLACK; txt = S_HINT; show_big = 0; break;
    }

    /* 反白块：整区填背景色 */
    svc_display_tft_area_t area = {UI_PROG_RESULT_X, UI_PROG_RESULT_Y,
                                   UI_PROG_RESULT_W, UI_PROG_RESULT_H};
    svc_display_tft_area_fill_all(&area, bg);

    /* 文字（白色居中）。成功/失败用 GB2312 16×16（s_chs），提示用同字体 */
    svc_display_tft_rect_t r;
    r.area.x = UI_PROG_RESULT_X; r.area.y = UI_PROG_RESULT_Y;
    r.area.width = UI_PROG_RESULT_W; r.area.height = UI_PROG_RESULT_H;
    r.edge_width = 0; r.edge_options = SVC_DISPLAY_TFT_EDGE_NO;
    svc_display_tft_color_t fg = (result == UI_PROG_RESULT_NONE) ? DEV_LCD_TFT_GRAY : DEV_LCD_TFT_WHITE;
    svc_display_tft_rect_text(&r, txt, s_chs, SVC_DISPLAY_TFT_ALIGN_CENTER, fg, bg, 1);
    (void)show_big;
}

/** 画分隔线 */
static void draw_separator(void)
{
    svc_display_tft_line_t ln;
    ln.point.x = 0; ln.point.y = UI_PROG_SEP_Y; ln.len = UI_LCD_W;
    svc_display_tft_x_line(&ln, DEV_LCD_TFT_GRAY);
}

/* ==================== 对外接口 ==================== */

void ui_view_program_init(font_mgr_t *ascii_font, font_mgr_t *chs_font)
{
    s_ascii = ascii_font;
    s_chs = chs_font;
    s_scroll_px = 0;
    s_scroll_divider = 0;
}

void ui_view_program_clear(void)
{
    svc_display_tft_clear(DEV_LCD_TFT_BLACK);
}

uint8_t ui_view_program_scroll_tick(const uint8_t *filename)
{
    if (!filename) return s_scroll_px;
    uint32_t w = font_mgr_get_width(s_ascii, filename, 1);
    if (w <= UI_PROG_FILENAME_W)
        return 0;   /* 不超宽，不滚 */

    if (++s_scroll_divider < SCROLL_TICK_DIV)
        return s_scroll_px;
    s_scroll_divider = 0;

    /* 滚动到「末尾后留 16px 空白」再回绕，形成循环 */
    uint16_t max_px = (uint16_t)(w - UI_PROG_FILENAME_W) + 16;
    s_scroll_px++;
    if (s_scroll_px > max_px)
        s_scroll_px = 0;
    return s_scroll_px;
}

void ui_view_program_draw(const ui_prog_state_t *state)
{
    if (!state) return;

    /* 文件名行 */
    draw_filename(state->filename);

    /* 分隔线 */
    draw_separator();

    /* 两行参数 */
    /* 行1：芯片: <型号> （单标签占整行，值 ASCII） */
    draw_kv(UI_PROG_PARAM1_Y, S_CHIP, state->chip_model,
            UI_PROG_COL1_X, UI_LCD_W - 2);

    /* 行2：次数: <n>（左列，值 ASCII）  保护: <开/关>（右列，值中文） */
    {
        char cnt[12];
        snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)state->burn_count);
        draw_kv(UI_PROG_PARAM2_Y, S_COUNT, (const uint8_t *)cnt,
                UI_PROG_COL1_X, UI_PROG_COL1_MAX);
        draw_kv(UI_PROG_PARAM2_Y, S_PROT, state->rdp ? S_ON : S_OFF,
                UI_PROG_COL2_X, UI_PROG_COL2_MAX);
    }

    /* 进行中：显示「烧录中」+ 进度条（无百分比）；完成：只显示结果 */
    if (state->progress > 0 && state->progress < 100) {
        draw_burning(state->progress);
    }

    /* 结果区：完成后显示成功/失败反白块；未开始显示操作提示 */
    draw_result(state->result);
}
