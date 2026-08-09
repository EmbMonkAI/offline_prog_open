/**
 * @file svc_display.c
 * @brief 显示服务层实现
 *
 * 提供画线、填充、矩形、文字渲染等显示服务。
 * 依赖设备层（dev_lcd、dev_font）和组件层（font_mgr）。
 *
 * LCD 设备和字体对象由应用层创建并传入，本层不初始化依赖模块。
 */
#include "svc_display.h"
#include "lcd/dev_lcd.h"
#include "font_mgr/font_mgr.h"
#include "font/dev_font.h"
#include <string.h>
#include <stdio.h>

#define GEN_LOG_MODULE  GEN_LOG_DISPLAY
#define GEN_LOG_TAG     "display"
#include "gen_log.h"

/** 保存应用层传入的 LCD 设备指针 */
static dev_lcd_dev_t *s_lcd;

uint8_t svc_display_init(dev_lcd_dev_t *lcd)
{
    if (!lcd)
        return 1;

    s_lcd = lcd;

    /* 初始化字库数据源 */
    if (dev_font_init() != 0) {
        gen_log_err("svc_display: font init fail\n");
        return 1;
    }

    return 0;
}

void svc_display_deinit(void)
{
    dev_font_deinit();
    s_lcd = NULL;
}

void svc_display_clear(uint8_t layer)
{
    dev_lcd_clear(s_lcd, layer);
}

void svc_display_refresh(uint8_t layer)
{
    dev_lcd_refresh(s_lcd, layer);
}

static uint8_t svc_display_point(uint8_t layer, svc_display_point_t *point, svc_display_color_t color)
{
    return dev_lcd_set_pixel(s_lcd, layer, point->x, point->y, color);
}

uint8_t svc_display_x_line(uint8_t layer, svc_display_line_t *line, svc_display_color_t color)
{
    svc_display_point_t point = {0};
    point.x = line->point.x;
    point.y = line->point.y;

    for (uint16_t i = 0; i < line->len; i++)
    {
        if (point.x >= SVC_DISPLAY_WIDTH)
            return 1;
        uint8_t ret = svc_display_point(layer, &point, color);
        if (ret != 0) return ret;
        point.x++;
    }
    return 0;
}

uint8_t svc_display_y_line(uint8_t layer, svc_display_line_t *line, svc_display_color_t color)
{
    svc_display_point_t point = {0};
    point.x = line->point.x;
    point.y = line->point.y;

    for (uint16_t i = 0; i < line->len; i++)
    {
        if (point.y >= SVC_DISPLAY_HEIGHT)
            return 1;
        uint8_t ret = svc_display_point(layer, &point, color);
        if (ret != 0) return ret;
        point.y++;
    }
    return 0;
}

uint8_t svc_display_area_fill_all(uint8_t layer, const svc_display_area_t *area, svc_display_color_t color)
{
    uint8_t page_data = (color == SVC_DISPLAY_COLOR_FRG) ? 0xff : 0;
    uint16_t j;
    uint16_t page_cnt = area->height / 8;
    uint8_t remain = area->height & 0x7;

    for (j = 0; j < page_cnt; j++)
    {
        uint8_t ret = dev_lcd_set_mult_same_page_pixel(s_lcd, layer, area->x, area->y + j * 8, 8, area->width, page_data);
        if (ret != 0) return ret;
    }
    if (remain)
    {
        uint8_t ret = dev_lcd_set_mult_same_page_pixel(s_lcd, layer, area->x, area->y + j * 8, remain, area->width, page_data);
        if (ret != 0) return ret;
    }
    return 0;
}

uint8_t svc_display_area_pixel(uint8_t layer, svc_display_area_t *area, const uint8_t *PixelData, svc_display_color_t Color)
{
    if (area == NULL)
        return 2;

    uint16_t page_cnt = area->height >> 3;
    uint8_t remain = area->height & 0x7;
    uint16_t i, j;

    for (i = 0; i < page_cnt; i++)
    {
        if (Color == SVC_DISPLAY_COLOR_FRG)
        {
            dev_lcd_set_mult_page_pixel(s_lcd, layer, area->x, area->y + i * 8, 8, area->width, PixelData);
            PixelData += area->width;
        }
        else
        {
            for (j = 0; j < area->width; j++, PixelData++)
            {
                dev_lcd_set_page_pixel(s_lcd, layer, area->x + j, area->y + i * 8, 8, SVC_DISPLAY_COLOR_REVERSE(*PixelData));
            }
        }
    }
    if (remain)
    {
        if (Color == SVC_DISPLAY_COLOR_FRG)
        {
            dev_lcd_set_mult_page_pixel(s_lcd, layer, area->x, area->y + page_cnt * 8, remain, area->width, PixelData);
        }
        else
        {
            for (j = 0; j < area->width; j++, PixelData++)
            {
                dev_lcd_set_page_pixel(s_lcd, layer, area->x + j, area->y + page_cnt * 8, remain, SVC_DISPLAY_COLOR_REVERSE(*PixelData));
            }
        }
    }
    return 0;
}

uint8_t svc_display_rect_fill_all(uint8_t layer, const svc_display_rect_t *Rect, svc_display_color_t color)
{
    svc_display_area_t tmp_area = {0};

    if (Rect == NULL)
        return 2;
    if ((Rect->area.width == 0) || (Rect->area.height == 0))
        return 1;

    tmp_area.x = Rect->area.x;
    tmp_area.y = Rect->area.y;
    tmp_area.width = Rect->area.width;
    tmp_area.height = Rect->area.height;
    svc_display_area_fill_all(layer, &tmp_area, color);

    if (Rect->edge_width == 0)
        return 0;

    if (SVC_DISPLAY_RECT_EDGE_IS_SET(Rect, SVC_DISPLAY_EDGE_TOP))
    {
        tmp_area.height = Rect->edge_width;
        svc_display_area_fill_all(layer, &tmp_area, SVC_DISPLAY_COLOR_REVERSE(color));
    }
    if (SVC_DISPLAY_RECT_EDGE_IS_SET(Rect, SVC_DISPLAY_EDGE_BOTTOM))
    {
        tmp_area.y = Rect->area.y + Rect->area.height - Rect->edge_width;
        svc_display_area_fill_all(layer, &tmp_area, SVC_DISPLAY_COLOR_REVERSE(color));
    }
    if (SVC_DISPLAY_RECT_EDGE_IS_SET(Rect, SVC_DISPLAY_EDGE_LEFT))
    {
        tmp_area.y = Rect->area.y + Rect->edge_width;
        tmp_area.width = Rect->edge_width;
        tmp_area.height = Rect->area.height - 2 * Rect->edge_width;
        svc_display_area_fill_all(layer, &tmp_area, SVC_DISPLAY_COLOR_REVERSE(color));
    }
    if (SVC_DISPLAY_RECT_EDGE_IS_SET(Rect, SVC_DISPLAY_EDGE_RIGHT))
    {
        tmp_area.x = Rect->area.x + Rect->area.width - Rect->edge_width;
        tmp_area.y = Rect->area.y + Rect->edge_width;
        tmp_area.width = Rect->edge_width;
        tmp_area.height = Rect->area.height - 2 * Rect->edge_width;
        svc_display_area_fill_all(layer, &tmp_area, SVC_DISPLAY_COLOR_REVERSE(color));
    }
    return 0;
}

uint8_t svc_display_rect_text(uint8_t layer, const svc_display_rect_t *Rect,
                            const uint8_t *String, font_mgr_t *font,
                            svc_display_align_t Align, svc_display_color_t Color, uint8_t SpacePixel)
{
    uint16_t disp_width = font_mgr_get_width(font, String, SpacePixel);
    uint16_t content_width = SVC_DISPLAY_RECT_CONTENT_WIDTH_GET(Rect);
    uint16_t content_height = SVC_DISPLAY_RECT_CONTENT_HEIGHT_GET(Rect);
    uint8_t font_data[128];
    uint16_t tmp_width;
    uint16_t font_width;
    svc_display_area_t area;

    uint8_t ret = 1;
    if ((ret = svc_display_rect_fill_all(layer, Rect, SVC_DISPLAY_COLOR_REVERSE(Color))) != 0)
        return ret;

    if (disp_width > content_width)
        disp_width = content_width;

    switch (Align)
    {
    case SVC_DISPLAY_ALIGN_LEFT:
        area.x = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT);
        break;
    case SVC_DISPLAY_ALIGN_RIGHT:
        area.x = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT) + content_width - disp_width;
        break;
    case SVC_DISPLAY_ALIGN_CENTER:
    default:
        area.x = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT) + (content_width - disp_width) / 2;
        break;
    }

    if (font->height > content_height)
        area.y = Rect->area.y + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_TOP);
    else
        area.y = Rect->area.y + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_TOP) + (content_height - font->height) / 2;
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
            svc_display_area_pixel(layer, &area, font_data, Color);
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

uint8_t svc_display_graph(uint8_t layer, const svc_display_rect_t *Rect,
                            const svc_display_graph_t *BMPRes,
                            svc_display_align_t Align, svc_display_color_t Color)
{
    uint16_t content_width = SVC_DISPLAY_RECT_CONTENT_WIDTH_GET(Rect);
    uint16_t content_height = SVC_DISPLAY_RECT_CONTENT_HEIGHT_GET(Rect);
    svc_display_area_t area;

    uint8_t ret = 1;
    if ((ret = svc_display_rect_fill_all(layer, Rect, SVC_DISPLAY_COLOR_REVERSE(Color))) != 0)
        return ret;

    area.width = BMPRes->width > content_width ? content_width : BMPRes->width;
    switch (Align)
    {
    case SVC_DISPLAY_ALIGN_LEFT:
        area.x = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT);
        break;
    case SVC_DISPLAY_ALIGN_RIGHT:
        area.x = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT) + content_width - area.width;
        break;
    case SVC_DISPLAY_ALIGN_CENTER:
    default:
        area.x = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT) + (content_width - area.width) / 2;
        break;
    }
    area.height = BMPRes->height > content_height ? content_height : BMPRes->height;
    area.y = Rect->area.y + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_TOP) + (content_height - area.height) / 2;

    ret = svc_display_area_pixel(layer, &area, (const uint8_t *)BMPRes->data, Color);
    return ret;
}

uint8_t svc_display_multiple_text(uint8_t layer, const svc_display_rect_t *Rect,
                                    const uint8_t *String, font_mgr_t *font,
                                    svc_display_align_t Align, svc_display_color_t Color)
{
    uint16_t lines, tmp_width;
    uint32_t font_code;
    uint16_t content_height = SVC_DISPLAY_RECT_CONTENT_HEIGHT_GET(Rect);
    uint8_t font_data[128];
    svc_display_area_t area;
    uint16_t rect_x_start = Rect->area.x + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT);
    uint16_t rect_width = SVC_DISPLAY_RECT_CONTENT_WIDTH_GET(Rect);
    uint8_t ret = 0;

    if ((ret = svc_display_rect_fill_all(layer, Rect, SVC_DISPLAY_COLOR_REVERSE(Color))) != 0)
        return ret;

    lines = content_height / font->height;
    if (content_height != lines * font->height)
    {
        while ((content_height - lines * font->height) * SVC_DISPLAY_LINE_INTERVAL_SPACE / lines < font->height)
        {
            lines--;
            if (lines < 1)
                return 0;
        }
    }
    area.x = rect_x_start;
    area.y = Rect->area.y + SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_TOP);
    area.height = font->height;

    for (uint8_t i = 0; i < lines; i++)
    {
        const uint8_t *current_str = String;
        uint16_t total_width = 0;

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

        if (Align == SVC_DISPLAY_ALIGN_CENTER)
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
                memset(font_data, 0xff, tmp_width / 8);
            }
            if (current_x + tmp_width > rect_x_start + rect_width)
            {
                String = current_str;
                break;
            }
            area.width = tmp_width;
            if ((ret = svc_display_area_pixel(layer, &area, font_data, Color)) != 0)
                return ret;
            current_x += tmp_width;
            current_str = font_mgr_get_next(font, current_str);
            area.x = current_x;
        }
        area.y += content_height / lines;
    }
    return 0;
}
