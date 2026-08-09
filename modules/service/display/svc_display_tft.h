#ifndef SVC_DISPLAY_TFT_H
#define SVC_DISPLAY_TFT_H

/**
 * @file svc_display_tft.h
 * @brief TFT 显示服务层接口（RGB565）
 *
 * 提供画点、画线、填充、矩形、文字渲染、位图等显示服务。
 * 依赖设备层（dev_lcd_tft、dev_font）和组件层（font_mgr）。
 *
 * 与单色 svc_display 的对应关系（语义一致，命名加 _tft 后缀，颜色模型改为 RGB565）：
 *   svc_display_x_line        → svc_display_tft_x_line
 *   svc_display_y_line        → svc_display_tft_y_line
 *   svc_display_area_fill_all → svc_display_tft_area_fill_all
 *   svc_display_area_pixel    → svc_display_tft_area_pixel   (1-bit 位图/字体点阵 blit)
 *   svc_display_rect_fill_all → svc_display_tft_rect_fill_all
 *   svc_display_rect_text     → svc_display_tft_rect_text
 *   svc_display_graph         → svc_display_tft_graph
 *   svc_display_multiple_text → svc_display_tft_multiple_text
 *
 * 本层不初始化依赖模块，LCD 设备和字体对象由应用层创建并传入。
 * 所有操作写入 dev_lcd_tft 帧缓冲，需调用 dev_lcd_tft_refresh() 才显示。
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "dev_lcd_tft.h"       /* Keil: include 路径直接指向 lcd/;PC CMake: 两种风格均可见 */
#include "font_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 颜色类型（直接复用 dev_lcd_tft 的 RGB565）
 * ================================================================ */

typedef dev_lcd_tft_color_t svc_display_tft_color_t;

/** 透明色标记：用于文字/位图背景。设为此值时 0 像素不绘制（保留底图） */
#define SVC_DISPLAY_TFT_TRANSPARENT  0x00010000u   /* 超出 RGB565 范围，用作哨兵 */

/* ================================================================
 * 几何类型（结构与单色 svc_display 对齐，便于移植）
 * ================================================================ */

typedef enum __attribute__((packed))
{
    SVC_DISPLAY_TFT_ALIGN_LEFT = 0,
    SVC_DISPLAY_TFT_ALIGN_CENTER = 1,
    SVC_DISPLAY_TFT_ALIGN_RIGHT = 2,
} svc_display_tft_align_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
} svc_display_tft_point_t;

typedef struct
{
    svc_display_tft_point_t point;
    uint16_t len;
} svc_display_tft_line_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} svc_display_tft_area_t;

typedef enum __attribute__((packed))
{
    SVC_DISPLAY_TFT_EDGE_NO     = 0x0,
    SVC_DISPLAY_TFT_EDGE_LEFT   = 0x1,
    SVC_DISPLAY_TFT_EDGE_RIGHT  = 0x2,
    SVC_DISPLAY_TFT_EDGE_TOP    = 0x4,
    SVC_DISPLAY_TFT_EDGE_BOTTOM = 0x8,
    SVC_DISPLAY_TFT_EDGE_ALL    = 0xf,
} svc_display_tft_edge_options_t;

typedef struct
{
    svc_display_tft_area_t area;
    uint16_t edge_width;
    svc_display_tft_edge_options_t edge_options;
} svc_display_tft_rect_t;

typedef struct
{
    uint8_t width;
    uint8_t height;
    const char *data;
} svc_display_tft_graph_t;

#define SVC_DISPLAY_TFT_RECT_EDGE_IS_SET(Rect, EdgeType)        ((Rect)->edge_options & (EdgeType))
#define SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, EdgeType)     (SVC_DISPLAY_TFT_RECT_EDGE_IS_SET(Rect, EdgeType) ? (Rect)->edge_width : 0)
#define SVC_DISPLAY_TFT_RECT_CONTENT_WIDTH_GET(Rect)            ((Rect)->area.width - SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_LEFT) - SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_RIGHT))
#define SVC_DISPLAY_TFT_RECT_CONTENT_HEIGHT_GET(Rect)           ((Rect)->area.height - SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_TOP) - SVC_DISPLAY_TFT_RECT_EDGE_WIDTH_GET(Rect, SVC_DISPLAY_TFT_EDGE_BOTTOM))

#define SVC_DISPLAY_TFT_LINE_INTERVAL_SPACE        8

/* ================================================================
 * 对外接口
 * ================================================================ */

/**
 * @brief 初始化 TFT 显示服务
 * @param lcd  TFT 设备指针（应用层已初始化）
 * @return 0 成功，非0 失败（同时初始化字库数据源）
 */
uint8_t svc_display_tft_init(dev_lcd_tft_dev_t *lcd);

void    svc_display_tft_deinit(void);

/**
 * @brief 清屏（填充指定颜色到帧缓冲，不刷屏）
 */
void    svc_display_tft_clear(svc_display_tft_color_t color);

/**
 * @brief 画单个像素
 */
uint8_t svc_display_tft_point(svc_display_tft_point_t *point, svc_display_tft_color_t color);

/**
 * @brief 画水平线
 */
uint8_t svc_display_tft_x_line(svc_display_tft_line_t *line, svc_display_tft_color_t color);

/**
 * @brief 画垂直线
 */
uint8_t svc_display_tft_y_line(svc_display_tft_line_t *line, svc_display_tft_color_t color);

/**
 * @brief 用指定颜色填充整个区域
 */
uint8_t svc_display_tft_area_fill_all(const svc_display_tft_area_t *area, svc_display_tft_color_t color);

/**
 * @brief 把 1-bit 位图（列优先页格式，与单色字体/位图一致）blit 到区域
 *
 * PixelData 布局：每列从上到下，每 8 行组成 1 字节（高位在上），与现有
 * font_mgr 字体点阵、单色 svc_display_area_pixel 输入格式完全一致。
 * 前景色 fg 画 1 像素，0 像素画 bg；bg=SVC_DISPLAY_TFT_TRANSPARENT 时透明。
 */
uint8_t svc_display_tft_area_pixel(svc_display_tft_area_t *area, const uint8_t *PixelData,
                                   svc_display_tft_color_t fg, svc_display_tft_color_t bg);

/**
 * @brief 填充矩形（含边框）：边框区画 edge_color，内容区画 content_color
 */
uint8_t svc_display_tft_rect_fill_all(const svc_display_tft_rect_t *Rect,
                                      svc_display_tft_color_t edge_color,
                                      svc_display_tft_color_t content_color);

/**
 * @brief 在矩形内绘制文字（自动对齐、居中）
 * @param Rect     矩形（边框部分填 bg，文字画 fg）
 * @param String   字符串（ASCII 或 GB2312）
 * @param font     字体对象
 * @param Align    对齐方式
 * @param fg       文字前景色
 * @param bg       背景色（边框+文字间隙）
 * @param SpacePixel 字符间距（像素）
 */
uint8_t svc_display_tft_rect_text(const svc_display_tft_rect_t *Rect,
                                  const uint8_t *String, font_mgr_t *font,
                                  svc_display_tft_align_t Align,
                                  svc_display_tft_color_t fg,
                                  svc_display_tft_color_t bg, uint8_t SpacePixel);

/**
 * @brief 在矩形内绘制 1-bit 位图（自动对齐、居中）
 */
uint8_t svc_display_tft_graph(const svc_display_tft_rect_t *Rect,
                              const svc_display_tft_graph_t *BMPRes,
                              svc_display_tft_align_t Align,
                              svc_display_tft_color_t fg,
                              svc_display_tft_color_t bg);

/**
 * @brief 在矩形内绘制多行文字（自动换行 + 行距）
 */
uint8_t svc_display_tft_multiple_text(const svc_display_tft_rect_t *Rect,
                                      const uint8_t *String, font_mgr_t *font,
                                      svc_display_tft_align_t Align,
                                      svc_display_tft_color_t fg,
                                      svc_display_tft_color_t bg);

#ifdef __cplusplus
}
#endif

#endif /* SVC_DISPLAY_TFT_H */
