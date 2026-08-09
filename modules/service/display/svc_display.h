#ifndef SVC_DISPLAY_H
#define SVC_DISPLAY_H

/**
 * @file svc_display.h
 * @brief 显示服务层接口
 *
 * 提供画线、填充、矩形、文字渲染等显示服务。
 * 依赖设备层（dev_lcd、dev_font）和组件层（font_mgr）。
 *
 * 本层不初始化依赖模块，LCD 设备和字体对象由应用层创建并传入。
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lcd/dev_lcd.h"
#include "font_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 显示参数定义（以 dev_lcd.h 为准）
 * ================================================================ */

/** 双缓冲索引 */
#define SVC_DISPLAY_BUF_BASE    DEV_LCD_BUF_BASE
#define SVC_DISPLAY_BUF_TOP     DEV_LCD_BUF_TOP

/** LCD 尺寸 */
#define SVC_DISPLAY_WIDTH       DEV_LCD_MAX_WIDTH
#define SVC_DISPLAY_HEIGHT      DEV_LCD_MAX_HEIGHT

typedef enum __attribute__((packed))
{
    SVC_DISPLAY_COLOR_BKG = 0x00,
    SVC_DISPLAY_COLOR_FRG = 0xff,
} svc_display_color_t;

#define SVC_DISPLAY_COLOR_REVERSE(Color)    ((Color) ^ 0xff)

typedef enum __attribute__((packed))
{
    SVC_DISPLAY_ALIGN_LEFT = 0,
    SVC_DISPLAY_ALIGN_CENTER = 1,
    SVC_DISPLAY_ALIGN_RIGHT = 2,
} svc_display_align_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
} svc_display_point_t;

typedef struct
{
    svc_display_point_t point;
    uint16_t len;
} svc_display_line_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} svc_display_area_t;

typedef enum __attribute__((packed))
{
    SVC_DISPLAY_EDGE_NO     = 0x0,
    SVC_DISPLAY_EDGE_LEFT   = 0x1,
    SVC_DISPLAY_EDGE_RIGHT  = 0x2,
    SVC_DISPLAY_EDGE_TOP    = 0x4,
    SVC_DISPLAY_EDGE_BOTTOM = 0x8,
    SVC_DISPLAY_EDGE_ALL    = 0xf,
} svc_display_edge_options_t;

typedef struct
{
    svc_display_area_t area;
    uint16_t edge_width;
    svc_display_edge_options_t edge_options;
} svc_display_rect_t;

#define SVC_DISPLAY_RECT_EDGE_IS_SET(Rect, EdgeType)        ((Rect)->edge_options & (EdgeType))
#define SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, EdgeType)     (SVC_DISPLAY_RECT_EDGE_IS_SET(Rect, EdgeType) ? (Rect)->edge_width : 0)
#define SVC_DISPLAY_RECT_CONTENT_WIDTH_GET(Rect)            ((Rect)->area.width - SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_LEFT) - SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_RIGHT))
#define SVC_DISPLAY_RECT_CONTENT_HEIGHT_GET(Rect)           ((Rect)->area.height - SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_TOP) - SVC_DISPLAY_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_EDGE_BOTTOM))

typedef struct
{
    uint8_t width;
    uint8_t height;
    const char *data;
} svc_display_graph_t;

/* ================================================================
 * 对外接口
 * ================================================================ */

/**
 * @brief 初始化显示服务
 *
 * 保存 LCD 设备指针，由应用层负责初始化依赖模块。
 *
 * @param lcd LCD 设备指针（应用层已初始化）
 * @return 0 成功，非0 失败
 */
uint8_t svc_display_init(dev_lcd_dev_t *lcd);

void    svc_display_deinit(void);
void    svc_display_clear(uint8_t layer);
void    svc_display_refresh(uint8_t layer);

uint8_t svc_display_x_line(uint8_t layer, svc_display_line_t *line, svc_display_color_t color);
uint8_t svc_display_y_line(uint8_t layer, svc_display_line_t *line, svc_display_color_t color);
uint8_t svc_display_area_fill_all(uint8_t layer, const svc_display_area_t *area, svc_display_color_t color);
uint8_t svc_display_area_pixel(uint8_t layer, svc_display_area_t *area, const uint8_t *PixelData, svc_display_color_t Color);
uint8_t svc_display_rect_fill_all(uint8_t layer, const svc_display_rect_t *Rect, svc_display_color_t color);

uint8_t svc_display_rect_text(uint8_t layer, const svc_display_rect_t *Rect,
                            const uint8_t *String, font_mgr_t *font,
                            svc_display_align_t Align, svc_display_color_t Color, uint8_t SpacePixel);

uint8_t svc_display_graph(uint8_t layer, const svc_display_rect_t *Rect,
                            const svc_display_graph_t *BMPRes,
                            svc_display_align_t Align, svc_display_color_t Color);

#define SVC_DISPLAY_LINE_INTERVAL_SPACE        8

uint8_t svc_display_multiple_text(uint8_t layer, const svc_display_rect_t *Rect,
                                    const uint8_t *String, font_mgr_t *font,
                                    svc_display_align_t Align, svc_display_color_t Color);

#ifdef __cplusplus
}
#endif

#endif
