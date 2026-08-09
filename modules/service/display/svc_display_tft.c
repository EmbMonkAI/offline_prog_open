/**
 * @file svc_display_tft.c
 * @brief TFT 显示服务层实现（RGB565）
 *
 * 提供画点、画线、填充、矩形、文字渲染、位图等显示服务。
 * 算法逻辑移植自单色 svc_display.c（对齐/换行/行距计算一致），
 * 区别仅在像素写入：单色用 1-bit 页寻址（dev_lcd_set_mult_page_pixel 等），
 * 本层用 RGB565 帧缓冲（dev_lcd_tft_set_pixel）。
 *
 * 1-bit 位图/字体点阵的解包：
 *   现有 font_mgr 字体和单色位图都是"列优先、纵向 8 像素打包"格式
 *   （高位在上：bit7=最上一格）。本层 area_pixel() 按此格式解包，
 *   1→前景色，0→背景色（背景为透明哨兵则跳过）。
 */

#include "svc_display_tft.h"
#include "dev_font.h"           /* Keil: include 路径直接指向 font/;PC CMake 两种风格均可见 */
#include <string.h>
#include <stdio.h>

#define GEN_LOG_MODULE  GEN_LOG_DISPLAY
#define GEN_LOG_TAG     "display-tft"
#include "gen_log.h"

/** 保存应用层传入的 TFT 设备指针 */
static dev_lcd_tft_dev_t *s_lcd;

/* ==================== 初始化 ==================== */

uint8_t svc_display_tft_init(dev_lcd_tft_dev_t *lcd)
{
    if (!lcd)
        return 1;

    s_lcd = lcd;

    /* 初始化字库数据源（与单色版共用同一字库文件/读取实现） */
    if (dev_font_init() != 0) {
        gen_log_err("svc_display_tft: font init fail\n");
        return 1;
    }

    return 0;
}

void svc_display_tft_deinit(void)
{
    dev_font_deinit();
    s_lcd = NULL;
}

void svc_display_tft_clear(svc_display_tft_color_t color)
{
    dev_lcd_tft_clear(s_lcd, color);
}

/* ==================== 基本图元 ==================== */

uint8_t svc_display_tft_point(svc_display_tft_point_t *point, svc_display_tft_color_t color)
{
    if (!point)
        return 2;
    return dev_lcd_tft_set_pixel(s_lcd, point->x, point->y, color);
}

uint8_t svc_display_tft_x_line(svc_display_tft_line_t *line, svc_display_tft_color_t color)
{
    if (!line)
        return 2;
    svc_display_tft_point_t point = {0};
    point.x = line->point.x;
    point.y = line->point.y;

    for (uint16_t i = 0; i < line->len; i++)
    {
        if (point.x >= DEV_LCD_TFT_WIDTH)
            return 1;
        uint8_t ret = svc_display_tft_point(&point, color);
        if (ret != 0) return ret;
        point.x++;
    }
    return 0;
}

uint8_t svc_display_tft_y_line(svc_display_tft_line_t *line, svc_display_tft_color_t color)
{
    if (!line)
        return 2;
    svc_display_tft_point_t point = {0};
    point.x = line->point.x;
    point.y = line->point.y;

    for (uint16_t i = 0; i < line->len; i++)
    {
        if (point.y >= DEV_LCD_TFT_HEIGHT)
            return 1;
        uint8_t ret = svc_display_tft_point(&point, color);
        if (ret != 0) return ret;
        point.y++;
    }
    return 0;
}

uint8_t svc_display_tft_area_fill_all(const svc_display_tft_area_t *area, svc_display_tft_color_t color)
{
    if (!area)
        return 2;

    for (uint16_t y = 0; y < area->height; y++)
    {
        for (uint16_t x = 0; x < area->width; x++)
        {
            uint16_t px = area->x + x;
            uint16_t py = area->y + y;
            if (px >= DEV_LCD_TFT_WIDTH || py >= DEV_LCD_TFT_HEIGHT)
                continue;
            dev_lcd_tft_set_pixel(s_lcd, px, py, color);
        }
    }
    return 0;
}

/**
 * svc_display_tft_area_pixel — 1-bit 位图/字体点阵 blit
 *
 * PixelData 格式（与单色一致，列优先页格式）：
 *   数据按"页"组织，每页 8 行。对 area->height 行，共 height/8(向上取整) 页。
 *   每页有 area->width 个字节，每个字节的 8 位对应一列的 8 个垂直像素（高位在上）。
 *   即 PixelData[page * width + col] 的 bit k → 像素 (area.x+col, area.y + page*8 + k)。
 *
 * fg/bg：
 *   1 像素 → fg；0 像素 → bg；bg=透明哨兵时跳过（保留底图）。
 */
uint8_t svc_display_tft_area_pixel(svc_display_tft_area_t *area, const uint8_t *PixelData,
                                   svc_display_tft_color_t fg, svc_display_tft_color_t bg)
{
    if (area == NULL || PixelData == NULL)
        return 2;

    uint8_t transparent = (bg == (svc_display_tft_color_t)SVC_DISPLAY_TFT_TRANSPARENT) ? 1 : 0;
    uint16_t page_cnt = area->height / 8;
    uint8_t  remain   = area->height & 0x7;

    for (uint16_t page = 0; page < page_cnt; page++)
    {
        for (uint16_t col = 0; col < area->width; col++)
        {
            uint8_t bits = PixelData[page * area->width + col];
            uint16_t base_x = area->x + col;
            if (base_x >= DEV_LCD_TFT_WIDTH) continue;

            for (uint8_t bit = 0; bit < 8; bit++)
            {
                uint16_t py = area->y + page * 8 + bit;
                if (py >= DEV_LCD_TFT_HEIGHT) break;

                if (bits & (1 << bit))   /* 低位在上：bit0=最上一格（与单色页格式一致） */
                    dev_lcd_tft_set_pixel(s_lcd, base_x, py, fg);
                else if (!transparent)
                    dev_lcd_tft_set_pixel(s_lcd, base_x, py, bg);
            }
        }
    }

    /* 不足 8 行的末页 */
    if (remain)
    {
        for (uint16_t col = 0; col < area->width; col++)
        {
            uint8_t bits = PixelData[page_cnt * area->width + col];
            uint16_t base_x = area->x + col;
            if (base_x >= DEV_LCD_TFT_WIDTH) continue;

            for (uint8_t bit = 0; bit < remain; bit++)
            {
                uint16_t py = area->y + page_cnt * 8 + bit;
                if (py >= DEV_LCD_TFT_HEIGHT) break;

                if (bits & (1 << bit))   /* 低位在上 */
                    dev_lcd_tft_set_pixel(s_lcd, base_x, py, fg);
                else if (!transparent)
                    dev_lcd_tft_set_pixel(s_lcd, base_x, py, bg);
            }
        }
    }
    return 0;
}

uint8_t svc_display_tft_rect_fill_all(const svc_display_tft_rect_t *Rect,
                                      svc_display_tft_color_t edge_color,
                                      svc_display_tft_color_t content_color)
{
    svc_display_tft_area_t tmp = {0};

    if (Rect == NULL)
        return 2;

    /* 整个矩形先填边框色 */
    tmp.x = Rect->area.x;
    tmp.y = Rect->area.y;
    tmp.width = Rect->area.width;
    tmp.height = Rect->area.height;
    if (svc_display_tft_area_fill_all(&tmp, edge_color) != 0)
        return 1;

    /* 内容区填内容色 */
    uint16_t el = SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT);
    uint16_t et = SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_TOP);
    uint16_t cw = SVC_DISPLAY_TFT_RECT_CONTENT_WIDTH_GET(Rect);
    uint16_t ch = SVC_DISPLAY_TFT_RECT_CONTENT_HEIGHT_GET(Rect);
    if (cw && ch)
    {
        tmp.x = Rect->area.x + el;
        tmp.y = Rect->area.y + et;
        tmp.width = cw;
        tmp.height = ch;
        if (svc_display_tft_area_fill_all(&tmp, content_color) != 0)
            return 1;
    }
    return 0;
}

/* ==================== 文字渲染 ==================== */

/**
 * svc_display_tft_rect_text — 在矩形内绘制单行文字
 *
 * 逻辑移植自 svc_display_rect_text：先算文字像素宽，按对齐方式定 x，
 * 逐字符取点阵并 area_pixel blit；矩形先填背景色。
 */
uint8_t svc_display_tft_rect_text(const svc_display_tft_rect_t *Rect,
                                  const uint8_t *String, font_mgr_t *font,
                                  svc_display_tft_align_t Align,
                                  svc_display_tft_color_t fg,
                                  svc_display_tft_color_t bg, uint8_t SpacePixel)
{
    if (!Rect || !String || !font)
        return 2;

    uint16_t disp_width    = font_mgr_get_width(font, String, SpacePixel);
    uint16_t content_width = SVC_DISPLAY_TFT_RECT_CONTENT_WIDTH_GET(Rect);
    uint16_t content_height= SVC_DISPLAY_TFT_RECT_CONTENT_HEIGHT_GET(Rect);
    uint8_t  font_data[128];
    uint16_t tmp_width, font_width;
    svc_display_tft_area_t area;
    uint16_t rect_x_start = Rect->area.x + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT);

    /* 矩形先填背景色（边框+间隙） */
    if (svc_display_tft_rect_fill_all(Rect, bg, bg) != 0)
        return 1;

    if (disp_width > content_width)
        disp_width = content_width;

    switch (Align)
    {
    case SVC_DISPLAY_TFT_ALIGN_LEFT:
        area.x = rect_x_start;
        break;
    case SVC_DISPLAY_TFT_ALIGN_RIGHT:
        area.x = rect_x_start + content_width - disp_width;
        break;
    case SVC_DISPLAY_TFT_ALIGN_CENTER:
    default:
        area.x = rect_x_start + (content_width - disp_width) / 2;
        break;
    }

    if (font->height > content_height)
        area.y = Rect->area.y + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_TOP);
    else
        area.y = Rect->area.y + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_TOP)
                 + (content_height - font->height) / 2;
    area.height = font->height;

    tmp_width = 0;
    do
    {
        uint32_t code = font_mgr_get_code(font, String);
        font_width = font_mgr_get_pixel_data(font, code, font_data);
        tmp_width += font_width;

        if (tmp_width <= disp_width)
        {
            area.width = font_width;
            svc_display_tft_area_pixel(&area, font_data, fg, bg);
            tmp_width += SpacePixel;
            area.x += font_width + SpacePixel;
            String = font_mgr_get_next(font, String);
        }
        else
        {
            break;
        }
    } while (1);

    return 0;
}

uint8_t svc_display_tft_graph(const svc_display_tft_rect_t *Rect,
                              const svc_display_tft_graph_t *BMPRes,
                              svc_display_tft_align_t Align,
                              svc_display_tft_color_t fg,
                              svc_display_tft_color_t bg)
{
    if (!Rect || !BMPRes)
        return 2;

    uint16_t content_width  = SVC_DISPLAY_TFT_RECT_CONTENT_WIDTH_GET(Rect);
    uint16_t content_height = SVC_DISPLAY_TFT_RECT_CONTENT_HEIGHT_GET(Rect);
    svc_display_tft_area_t area;

    if (svc_display_tft_rect_fill_all(Rect, bg, bg) != 0)
        return 1;

    area.width = BMPRes->width > content_width ? content_width : BMPRes->width;
    switch (Align)
    {
    case SVC_DISPLAY_TFT_ALIGN_LEFT:
        area.x = Rect->area.x + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT);
        break;
    case SVC_DISPLAY_TFT_ALIGN_RIGHT:
        area.x = Rect->area.x + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT)
                 + content_width - area.width;
        break;
    case SVC_DISPLAY_TFT_ALIGN_CENTER:
    default:
        area.x = Rect->area.x + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT)
                 + (content_width - area.width) / 2;
        break;
    }
    area.height = BMPRes->height > content_height ? content_height : BMPRes->height;
    area.y = Rect->area.y + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_TOP)
             + (content_height - area.height) / 2;

    return svc_display_tft_area_pixel(&area, (const uint8_t *)BMPRes->data, fg, bg);
}

/**
 * svc_display_tft_multiple_text — 多行文字（自动换行 + 行距）
 *
 * 逻辑移植自 svc_display_multiple_text：
 *   - 按矩形高度和字体高度算最多行数 lines
 *   - 每行先扫描出一个不超宽的子串，按对齐画
 *   - 遇 '\n' 强制换行，'\r' 跳过
 *   - 行高 = content_height / lines
 */
uint8_t svc_display_tft_multiple_text(const svc_display_tft_rect_t *Rect,
                                      const uint8_t *String, font_mgr_t *font,
                                      svc_display_tft_align_t Align,
                                      svc_display_tft_color_t fg,
                                      svc_display_tft_color_t bg)
{
    if (!Rect || !String || !font)
        return 2;

    uint16_t lines, tmp_width;
    uint32_t font_code;
    uint16_t content_height = SVC_DISPLAY_TFT_RECT_CONTENT_HEIGHT_GET(Rect);
    uint8_t  font_data[128];
    svc_display_tft_area_t area;
    uint16_t rect_x_start = Rect->area.x + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT);
    uint16_t rect_width   = SVC_DISPLAY_TFT_RECT_CONTENT_WIDTH_GET(Rect);
    uint8_t  ret = 0;

    if (svc_display_tft_rect_fill_all(Rect, bg, bg) != 0)
        return 1;

    lines = content_height / font->height;
    if (content_height != lines * font->height)
    {
        while ((content_height - lines * font->height) * SVC_DISPLAY_TFT_LINE_INTERVAL_SPACE / lines < font->height)
        {
            lines--;
            if (lines < 1)
                return 0;
        }
    }
    area.x = rect_x_start;
    area.y = Rect->area.y + SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_TOP);
    area.height = font->height;

    for (uint16_t i = 0; i < lines; i++)
    {
        const uint8_t *current_str = String;
        uint16_t total_width = 0;

        /* 扫描一行：累计宽度直到超宽或遇换行 */
        while (1)
        {
            font_code = font_mgr_get_code(font, current_str);
            if (font_code == '\0' || font_code == '\n')
                break;
            if (font_code == '\r')
            {
                current_str = font_mgr_get_next(font, current_str);
                continue;
            }
            tmp_width = font_mgr_get_pixel_data(font, font_code, font_data);
            if (!tmp_width)
                tmp_width = font->width;
            total_width += tmp_width;
            current_str = font_mgr_get_next(font, current_str);
        }

        if (Align == SVC_DISPLAY_TFT_ALIGN_CENTER)
            area.x = rect_x_start + (rect_width - total_width) / 2;
        else
            area.x = rect_x_start;

        current_str = String;
        uint16_t current_x = area.x;
        while (1)
        {
            font_code = font_mgr_get_code(font, current_str);
            if (font_code == '\0')
                return 0;
            if (font_code == '\r')
            {
                current_str = font_mgr_get_next(font, current_str);
                continue;
            }
            if (font_code == '\n')
            {
                String = font_mgr_get_next(font, current_str);
                break;
            }
            tmp_width = font_mgr_get_pixel_data(font, font_code, font_data);
            if (!tmp_width)
            {
                tmp_width = font->width;
                memset(font_data, 0xFF, tmp_width / 8);
            }
            if (current_x + tmp_width > rect_x_start + rect_width)
            {
                String = current_str;
                break;
            }
            area.width = tmp_width;
            if ((ret = svc_display_tft_area_pixel(&area, font_data, fg, bg)) != 0)
                return ret;
            current_x += tmp_width;
            current_str = font_mgr_get_next(font, current_str);
            area.x = current_x;
        }
        area.y += content_height / lines;
    }
    return 0;
}
