/**
 * @brief LCD显示模块稳定层（硬件无关）
 *
 * 此文件实现LCD像素缓冲区管理和脏区域刷新逻辑。
 * 不涉及任何硬件操作，所有硬件操作通过dev_lcd_dev_t中的函数指针完成。
 *
 * 缓冲区布局：
 *   s_buf[2][128][8] — 双缓冲区，每组128列×8页（适配128×64 LCD）
 *   s_dirty[2][8]    — 每组缓冲区对应8个page的脏区域标记
 *
 * 脏区域机制：
 *   每次调用dev_lcd_set_pixel/set_page_pixel等函数修改像素时，
 *   自动标记被修改列所在的page为"脏"。
 *   dev_lcd_refresh()时只将脏区域的数据通过hw_write_region()发送到LCD控制器，
 *   大幅减少SPI传输量（全屏刷新=1024字节，局部刷新可能只需几十字节）。
 *
 * 页寻址原理（SSD1306等LCD控制器常用）：
 *   LCD屏幕按"页"组织，每页8个像素高。
 *   坐标(x, y)中：page = y / 8（第几页），bit = y % 8（页内第几位）。
 *   缓冲区s_buf[x][page]的每一位对应一个像素，高位在上。
 */

#include "dev_lcd.h"
#include <string.h>

/* ==================== 模块内部缓冲区（不对外暴露） ==================== */

/**
 * s_buf — 双缓冲区
 *   [0] = BUF_BASE（底层缓冲区，通常用于主画面绘制）
 *   [1] = BUF_TOP（顶层缓冲区，通常用于叠加层绘制）
 *   [x][page] = 第x列、第page页的8个垂直像素（1字节 = 8像素）
 */
static uint8_t          s_buf[2][DEV_LCD_MAX_WIDTH][DEV_LCD_MAX_PAGES];

/**
 * s_dirty — 脏区域标记
 *   [index][page].start_col / end_col 记录该page中被修改的列范围
 *   start_col > end_col 表示该page未被修改（无需刷新）
 */
static dev_lcd_dirty_t  s_dirty[2][DEV_LCD_MAX_PAGES];

/* ==================== 对外接口实现 ==================== */

int dev_lcd_init(dev_lcd_dev_t *dev)
{
    if (!dev || !dev->hw_init) return -1;
    return dev->hw_init(dev);
}

void dev_lcd_deinit(dev_lcd_dev_t *dev)
{
    if (dev && dev->hw_deinit)
        dev->hw_deinit(dev);
}

/* ==================== 内部辅助函数 ==================== */

/**
 * lcd_mark_col — 标记某列为脏
 *
 * 当某列的像素被修改时调用，将该列纳入脏区域范围。
 * 如果当前列号超出已有范围，则扩展start_col或end_col。
 */
static void lcd_mark_col(dev_lcd_dirty_t *d, uint8_t x, uint8_t max_width)
{
    if (x >= max_width) return;
    if (x < d->start_col) d->start_col = x;
    if (x > d->end_col)   d->end_col = x;
}

/**
 * lcd_clear_dirty — 清除脏标记（刷新完成后调用）
 *
 * 将start_col设为max_width、end_col设为0，
 * 使得 start_col > end_col，表示该page无脏数据。
 */
static void lcd_clear_dirty(dev_lcd_dirty_t *d, uint8_t max_width)
{
    d->start_col = max_width;
    d->end_col = 0;
}

/* ==================== 像素操作接口 ==================== */

/**
 * dev_lcd_set_pixel — 设置单个像素
 *
 * 原理：找到目标字节buf[x][page]，设置或清除对应bit位。
 *   page = y >> 3（y除以8取商）
 *   bit  = y & 0x07（y除以8取余）
 *   color=1时置位（黑色），color=0时清位（白色）
 */
uint8_t dev_lcd_set_pixel(dev_lcd_dev_t *dev, uint8_t index,
                          uint8_t x, uint8_t y, uint8_t color)
{
    if (!dev || index > 1 || x >= dev->width || y >= dev->height) return 1;

    uint8_t page = y >> 3;
    uint8_t bit  = y & 0x07;
    dev_lcd_dirty_t *d = &s_dirty[index][page];

    lcd_mark_col(d, x, dev->width);

    if (color)
        s_buf[index][x][page] |=  (1 << bit);
    else
        s_buf[index][x][page] &= ~(1 << bit);

    return 0;
}

/**
 * dev_lcd_set_page_pixel — 设置单列单页像素（1字节，8个垂直像素）
 *
 * 处理跨页情况：当y不在page边界上且h超出当前页时，
 * 将数据拆分为高h1位写入当前页、低h2位写入下一页。
 *
 * 示例：y=5, h=6 → bit_idx=5, h1=3, h2=3
 *   第1页写入bit[5:7]（3位），第2页写入bit[0:2]（3位）
 */
uint8_t dev_lcd_set_page_pixel(dev_lcd_dev_t *dev, uint8_t index,
                               uint8_t x, uint8_t y, uint8_t h, uint8_t data)
{
    if (!dev || index > 1 || x >= dev->width || y >= dev->height || h > 8) return 1;

    if (y + h > dev->height) h = dev->height - y;

    uint8_t page    = y >> 3;
    uint8_t bit_idx = y & 0x07;
    uint8_t bit_max = bit_idx + h;

    dev_lcd_dirty_t *d = &s_dirty[index][page];
    lcd_mark_col(d, x, dev->width);

    if (bit_max <= 8) {
        /* 不跨页：只修改当前页的对应位 */
        uint8_t mask = ((1 << h) - 1) << bit_idx;
        s_buf[index][x][page] = (s_buf[index][x][page] & ~mask) | ((data << bit_idx) & mask);
    }
    else {
        /* 跨页：拆分为当前页高位部分 + 下一页低位部分 */
        uint8_t h1    = 8 - bit_idx;       /* 当前页写入的位数 */
        uint8_t h2    = h - h1;            /* 下一页写入的位数 */
        uint8_t mask1 = (0xFF << bit_idx); /* 当前页掩码 */
        uint8_t mask2 = (0xFF >> (8 - h2));/* 下一页掩码 */

        s_buf[index][x][page]     = (s_buf[index][x][page]     & ~mask1) | ((data << bit_idx) & mask1);
        s_buf[index][x][page + 1] = (s_buf[index][x][page + 1] & ~mask2) | ((data >> h1) & mask2);

        lcd_mark_col(&s_dirty[index][page + 1], x, dev->width);
    }

    return 0;
}

/**
 * dev_lcd_set_mult_page_pixel — 批量设置多列单页像素（逐列写入不同数据）
 *
 * 逐列从data数组读取像素数据，写入对应列的指定page区域。
 * 适用于渲染图像、字库等需要逐列写入不同数据的场景。
 */
uint8_t dev_lcd_set_mult_page_pixel(dev_lcd_dev_t *dev, uint8_t index,
                                     uint8_t x, uint8_t y, uint8_t h,
                                     uint16_t w, const uint8_t *data)
{
    if (!dev || index > 1 || !data || x >= dev->width || y >= dev->height || h > 8)
        return 1;

    if (x + w > dev->width)  w = dev->width - x;
    if (y + h > dev->height) h = dev->height - y;

    uint8_t page    = y >> 3;
    uint8_t bit_idx = y & 0x07;
    uint8_t bit_max = bit_idx + h;
    uint8_t end_x   = x + w;
    dev_lcd_dirty_t *d = &s_dirty[index][page];

    lcd_mark_col(d, x, dev->width);
    lcd_mark_col(d, end_x - 1, dev->width);

    if (bit_max <= 8)
    {
        /* 不跨页：所有列只修改当前页 */
        uint8_t mask = ((1 << h) - 1) << bit_idx;
        for (uint16_t i = x; i < end_x; i++)
            s_buf[index][i][page] = (s_buf[index][i][page] & ~mask) | ((*data++ << bit_idx) & mask);
    }
    else
    {
        /* 跨页：每列拆分为当前页高位 + 下一页低位 */
        uint8_t h1    = 8 - bit_idx;
        uint8_t h2    = h - h1;
        uint8_t mask1 = (0xFF << bit_idx);
        uint8_t mask2 = (0xFF >> (8 - h2));

        lcd_mark_col(&s_dirty[index][page + 1], x, dev->width);
        lcd_mark_col(&s_dirty[index][page + 1], end_x - 1, dev->width);

        for (uint16_t i = x; i < end_x; i++)
        {
            uint8_t dt = *data++;
            s_buf[index][i][page]     = (s_buf[index][i][page]     & ~mask1) | ((dt << bit_idx) & mask1);
            s_buf[index][i][page + 1] = (s_buf[index][i][page + 1] & ~mask2) | ((dt >> h1)    & mask2);
        }
    }

    return 0;
}

/**
 * dev_lcd_set_mult_same_page_pixel — 批量填充多列单页像素（所有列写入相同数据）
 *
 * 所有列填充相同的data值，适用于清屏、画矩形、画背景等场景。
 * 比dev_lcd_set_mult_page_pixel效率更高，因为不需要逐列读取data数组。
 */
uint8_t dev_lcd_set_mult_same_page_pixel(dev_lcd_dev_t *dev, uint8_t index,
                                          uint8_t x, uint8_t y, uint8_t h,
                                          uint16_t w, uint8_t data)
{
    if (!dev || index > 1 || x >= dev->width || y >= dev->height || h > 8) return 1;

    if (x + w > dev->width)  w = dev->width - x;
    if (y + h > dev->height) h = dev->height - y;

    uint8_t page     = y >> 3;
    uint8_t bit_idx  = y & 0x07;
    uint8_t bit_max  = bit_idx + h;
    uint8_t end_x    = x + w;
    dev_lcd_dirty_t *d = &s_dirty[index][page];

    lcd_mark_col(d, x, dev->width);
    lcd_mark_col(d, end_x - 1, dev->width);

    if (bit_max <= 8)
    {
        /* 不跨页：预计算图案，所有列写入相同值 */
        uint8_t mask = ((1 << h) - 1) << bit_idx;
        uint8_t pat  = (data << bit_idx) & mask;
        for (uint16_t i = x; i < end_x; i++)
            s_buf[index][i][page] = (s_buf[index][i][page] & ~mask) | pat;
    }
    else
    {
        /* 跨页：预计算两个页的图案，所有列写入相同值 */
        uint8_t h1    = 8 - bit_idx;
        uint8_t h2    = h - h1;
        uint8_t mask1 = (0xFF << bit_idx);
        uint8_t mask2 = (0xFF >> (8 - h2));
        uint8_t pat1  = (data << bit_idx) & mask1;
        uint8_t pat2  = (data >> h1) & mask2;

        lcd_mark_col(&s_dirty[index][page + 1], x, dev->width);
        lcd_mark_col(&s_dirty[index][page + 1], end_x - 1, dev->width);

        for (uint16_t i = x; i < end_x; i++)
        {
            s_buf[index][i][page]     = (s_buf[index][i][page]     & ~mask1) | pat1;
            s_buf[index][i][page + 1] = (s_buf[index][i][page + 1] & ~mask2) | pat2;
        }
    }

    return 0;
}

/* ==================== 刷新与清屏 ==================== */

/**
 * refresh_tmp — 刷新临时缓冲区
 *
 * s_buf 内存布局为列优先（s_buf[x][page]），同一page的不同列不连续。
 * hw_write_region 期望连续的page数据，因此需要先拷贝到连续缓冲区。
 */
static uint8_t refresh_tmp[DEV_LCD_MAX_WIDTH];

void dev_lcd_refresh(dev_lcd_dev_t *dev, uint8_t index)
{
    if (!dev || index > 1 || !dev->hw_write_region) return;

    for (uint8_t page = 0; page < dev->pages; page++) {
        dev_lcd_dirty_t *d = &s_dirty[index][page];
        if (d->start_col > d->end_col) continue;

        uint16_t len = d->end_col - d->start_col + 1;
        for (uint16_t i = 0; i < len; i++)
            refresh_tmp[i] = s_buf[index][d->start_col + i][page];

        dev->hw_write_region(dev, page, d->start_col, refresh_tmp, len);

        lcd_clear_dirty(d, dev->width);
    }
}

/**
 * dev_lcd_clear — 清空缓冲区（全白）并标记所有区域为脏
 *
 * 清零整个缓冲区，然后将每个page的脏区域设为全范围(0 ~ width-1)，
 * 确保下次dev_lcd_refresh()时会全屏刷新。
 * 注意：此函数只清空缓冲区，不会立即更新屏幕，需配合dev_lcd_refresh()使用。
 */
void dev_lcd_clear(dev_lcd_dev_t *dev, uint8_t index)
{
    if (!dev || index > 1) return;

    memset(s_buf[index], 0, sizeof(s_buf[index]));
    for (uint8_t i = 0; i < dev->pages; i++) {
        s_dirty[index][i].start_col = 0;
        s_dirty[index][i].end_col = dev->width - 1;
    }
}
