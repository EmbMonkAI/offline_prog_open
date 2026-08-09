/**
 * @brief TFT LCD 显示模块稳定层实现（硬件无关）
 *
 * 本文件只做像素流的攒批发送，不涉及任何具体 LCD 控制器的命令。
 * 所有硬件操作通过 dev_lcd_tft_dev_t 中的 hw_init / hw_set_window / hw_write_pixels
 * 回调完成。真屏后端（ST7789）和 PC 模拟后端（SDL）各实现一组回调，
 * 编译时链接其一即可，稳定层代码完全一致。
 *
 * ==================== 渲染模型：run 流水直接渲染 ====================
 *
 * 240×240×2 = 115KB 全屏帧缓冲放不进 F401 的 64KB SRAM，故本模块**不设帧缓冲**：
 * set_pixel 一边被调用一边把像素推向屏幕。相邻写入大多连续（fill/画线/字模逐行 blit），
 * 攒成"行内 run"批量发送即可获得接近帧缓冲的传输效率：
 *
 *   s_run —— 当前正在攒的连续段：起始像素 (run_x, run_y)，已攒 run_len 个像素。
 *   set_pixel(x,y,c)：
 *     · (x,y) 与 run 末尾衔接（同行右邻，或满宽 run 的行尾接下一行行首）→ 追加进 run
 *     · 否则 → 先把 run 推屏（set_window + write_pixels），再开新 run
 *   refresh() → 把最后一个 run 推屏（流水收尾）
 *
 * ⚠️ 跨行续接仅在 run 起点为 x=0 时允许：LCD 窗口内像素流换行落点是**窗口左端**
 *   (win_x0, 下一行)，只有窗口满宽（win_x0==0）时才等于 (0, 下一行)。
 *
 * 行缓冲 s_run_buf 攒满（RUN_BUF_PIXELS）也立即推屏 —— run 长度上限与内存解耦。
 *
 * 单缓冲撕裂问题：LCD 控制器按窗口行序写 GRAM，一段 run 在发送瞬间逐行可见，
 * 但应用层"整屏重绘后再做别的事"的用法下，视觉上等同整帧刷新（128 时代实测无撕裂感）。
 *
 * ==================== get_pixel（读回）说明 ====================
 *
 * 无帧缓冲后读回需后端支持：dev->hw_read_pixel 回调（可选，模拟后端用 shadow_buf 实现）。
 * 未提供（真屏：ST7789 只写不读）时返回 0。PC 的 PPM dump 工具依赖此回调。
 *
 * 像素字节序：发送时按"大端"（高字节在前）拆分，匹配 ST7789 的 SPI 像素顺序。
 * （若换用小端控制器，只需调整 run_buf 的字节拆分顺序。）
 */

#include "dev_lcd_tft.h"
#include <string.h>

/* ==================== run 攒批状态（不对外暴露） ==================== */

/**
 * 行缓冲：一次最多攒 RUN_BUF_PIXELS 个像素（480B@240 宽整一行）。
 * 取屏宽而非 2×屏宽：任何 run 都不会超过一行长度。
 */
#define RUN_BUF_PIXELS  DEV_LCD_TFT_WIDTH
static uint8_t s_run_buf[RUN_BUF_PIXELS * 2];

/**
 * 单事务像素上限（§13.17）：跨行续接的 run 攒到 8 行（8×240=1920）即推屏。
 * 防超长事务丢行；同步决定窗口高度不会越过屏底。
 */
#define RUN_MAX_PIXELS  (8u * DEV_LCD_TFT_WIDTH)

static struct {
    uint8_t  active;                    /* 1=正在攒 run */
    uint16_t x, y;                      /* run 起始像素 */
    uint16_t next_x, next_y;            /* 下一个应写入的像素坐标（run 末尾+1） */
    uint16_t len;                       /* 已攒像素数 */
} s_run;

/* ==================== 内部辅助函数 ==================== */

/**
 * 把当前 run 推到屏幕：begin_write(run 矩形, CS↓) + write_pixels + end_write(CS↑)
 *
 * 窗口取 [run.x .. 屏宽-1] × [run.y .. 末行]。像素流在窗口内按**窗口宽**换行
 * （落点 = 窗口左端），故行跨度必须按 win_w 而非屏宽算。
 * 跨行 run 仅当 run.x==0（窗口满宽）时才成立 —— 见 start_run/append_run 的
 * "禁止跨行"逻辑；此处保证两种情况 y1 计算都正确。
 */
static void dev_lcd_tft_flush_run(dev_lcd_tft_dev_t *dev)
{
    if (!s_run.active || s_run.len == 0) {
        s_run.active = 0;
        return;
    }

    uint16_t win_w = (uint16_t)(DEV_LCD_TFT_WIDTH - s_run.x);
    dev->hw_begin_write(dev, s_run.x, s_run.y,
                       (uint16_t)(DEV_LCD_TFT_WIDTH - 1),
                       (uint16_t)(s_run.y + (s_run.len - 1) / win_w));
    dev->hw_write_pixels(dev, s_run_buf, (uint32_t)s_run.len * 2);
    dev->hw_end_write(dev);

    s_run.active = 0;
    s_run.len = 0;
}

/**
 * 更新"下一个期望像素"坐标。
 * 行尾时：run.x==0（满宽窗口）才允许续接到 (0,y+1) —— 控制器窗口换行落点是
 * 窗口左端 (win_x0,y+1)，只有 win_x0==0 时才等于 (0,y+1)。
 * 否则置哨兵 0xFFFF 使续接判断永远失败（下次 set_pixel 先 flush 再开新 run）。
 */
static void dev_lcd_tft_update_next(uint16_t x, uint16_t y)
{
    s_run.next_x = (uint16_t)(x + 1);
    s_run.next_y = y;
    if (s_run.next_x >= DEV_LCD_TFT_WIDTH) {
        if (s_run.x == 0) {
            s_run.next_x = 0;
            s_run.next_y = (uint16_t)(y + 1);
        } else {
            s_run.next_x = 0xFFFF;   /* 禁止跨行：窗口换行落点 ≠ (0,y+1) */
            s_run.next_y = 0xFFFF;
        }
    }
}

/** 开启新 run：起点 (x,y)，写入首像素 */
static void dev_lcd_tft_start_run(uint16_t x, uint16_t y, dev_lcd_tft_color_t c)
{
    s_run.active  = 1;
    s_run.x       = x;
    s_run.y       = y;
    s_run.len     = 1;
    dev_lcd_tft_update_next(x, y);
    s_run_buf[0] = (uint8_t)(c >> 8);           /* 大端：高字节在前 */
    s_run_buf[1] = (uint8_t)(c & 0xFF);
}

/** 向当前 run 追加一个像素（调用方保证衔接且未满） */
static void dev_lcd_tft_append_run(uint16_t x, uint16_t y, dev_lcd_tft_color_t c)
{
    uint16_t i = s_run.len;
    s_run_buf[i * 2]     = (uint8_t)(c >> 8);
    s_run_buf[i * 2 + 1] = (uint8_t)(c & 0xFF);
    s_run.len++;
    dev_lcd_tft_update_next(x, y);
}

/* ==================== 对外接口实现 ==================== */

int dev_lcd_tft_init(dev_lcd_tft_dev_t *dev)
{
    if (!dev || !dev->hw_init) return -1;

    s_run.active = 0;
    s_run.len = 0;

    return dev->hw_init(dev);
}

uint8_t dev_lcd_tft_set_pixel(dev_lcd_tft_dev_t *dev,
                              uint16_t x, uint16_t y, dev_lcd_tft_color_t color)
{
    if (x >= DEV_LCD_TFT_WIDTH || y >= DEV_LCD_TFT_HEIGHT) return 1;

    if (s_run.active && s_run.len < RUN_BUF_PIXELS
        && x == s_run.next_x && y == s_run.next_y) {
        dev_lcd_tft_append_run(x, y, color);        /* 衔接：追加 */
        /* 事务长度上限（§13.17）：跨行续接的 run 最多 8 行（1920 像素）就推一次。
         * 超长单事务（如 60 行色块 = 14400 像素）实测丢行（红区黑横线）；
         * 8 行与自检色带的分行粒度接近，事务几何更规整。 */
        if (s_run.len >= RUN_MAX_PIXELS)
            dev_lcd_tft_flush_run(dev);
    } else {
        dev_lcd_tft_flush_run(dev);                 /* 断续：先推旧的 */
        dev_lcd_tft_start_run(x, y, color);         /* 再开新 run */
    }
    return 0;
}

dev_lcd_tft_color_t dev_lcd_tft_get_pixel(dev_lcd_tft_dev_t *dev,
                                          uint16_t x, uint16_t y)
{
    if (!dev || x >= DEV_LCD_TFT_WIDTH || y >= DEV_LCD_TFT_HEIGHT) return 0;
    if (dev->hw_read_pixel)
        return dev->hw_read_pixel(dev, x, y);       /* 模拟后端：读 shadow_buf */
    return 0;                                       /* 真屏：只写不读 */
}

void dev_lcd_tft_clear(dev_lcd_tft_dev_t *dev, dev_lcd_tft_color_t color)
{
    if (!dev || !dev->hw_begin_write || !dev->hw_write_pixels || !dev->hw_end_write) return;

    /* 全屏清屏：行缓冲复用为常量行，逐行发送。
     * ⚠️ 分带事务（60 行一带）：实测这块杂牌屏对"一整条全屏 RAMWR 大事务"
     *    状态异常（§13.16：v4 清黑屏显示成上半白），240×60 分带与 §13.10 起
     *    验证可用的色块填充几何一致。 */
    for (uint16_t i = 0; i < DEV_LCD_TFT_WIDTH; i++) {
        s_run_buf[i * 2]     = (uint8_t)(color >> 8);
        s_run_buf[i * 2 + 1] = (uint8_t)(color & 0xFF);
    }
    for (uint16_t y0 = 0; y0 < DEV_LCD_TFT_HEIGHT; y0 += 60) {
        uint16_t y1 = (uint16_t)(y0 + 59);
        if (y1 >= DEV_LCD_TFT_HEIGHT) y1 = (uint16_t)(DEV_LCD_TFT_HEIGHT - 1);
        dev->hw_begin_write(dev, 0, y0, (uint16_t)(DEV_LCD_TFT_WIDTH - 1), y1);
        for (uint16_t row = y0; row <= y1; row++)
            dev->hw_write_pixels(dev, s_run_buf, (uint32_t)DEV_LCD_TFT_WIDTH * 2);
        dev->hw_end_write(dev);
    }

    s_run.active = 0;   /* run 已废弃（屏幕内容已整屏覆盖） */
    s_run.len = 0;
}

/**
 * dev_lcd_tft_refresh — 流水收尾：把最后一个未推的 run 发出去
 *
 * 直接渲染模型下大部分数据在 set_pixel 时已上屏，refresh 只负责收尾。
 * 语义保留：应用层"画完一组图元后统一 refresh"的调用习惯不变。
 */
void dev_lcd_tft_refresh(dev_lcd_tft_dev_t *dev)
{
    if (!dev || !dev->hw_begin_write || !dev->hw_write_pixels || !dev->hw_end_write) return;
    dev_lcd_tft_flush_run(dev);
}
