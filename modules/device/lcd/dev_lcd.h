#ifndef __DEV_LCD_H
#define __DEV_LCD_H

#include <stdint.h>
#include "bsp_spi_soft.h"

/**
 * @brief LCD显示模块对外接口（硬件无关）
 *
 * dev_lcd_dev_t 包含硬件配置（SPI、GPIO）和LCD控制器操作函数指针。
 * 像素缓冲区由模块内部管理，不对外暴露。
 * 更换LCD硬件时，只需在应用层重新定义hw_init/hw_deinit/hw_write_region函数并填充设备结构体。
 *
 * 像素存储原理（页寻址模式）：
 *   LCD按"页"组织，每页8个像素高（1字节 = 8个垂直像素）。
 *   坐标(x, y) 中：page = y / 8, bit = y % 8
 *   缓冲区布局：buf[x][page]，每个字节存储同一列、同一页的8个垂直像素。
 *
 * 双缓冲机制：
 *   BUF_BASE(0) 和 BUF_TOP(1) 两组缓冲区交替使用，
 *   一组用于绘制，另一组用于刷新到屏幕，避免画面撕裂。
 *
 * 脏区域标记（dirty region）：
 *   每个page记录本次修改涉及的最小/最大列号(start_col, end_col)，
 *   refresh时只刷新脏区域，减少SPI传输量。
 */

/* 像素颜色 */
#define DEV_LCD_BLACK        1
#define DEV_LCD_WHITE        0

/* 双缓冲索引 */
#define DEV_LCD_BUF_BASE    0   /* 底层缓冲区（通常用于主画面绘制） */
#define DEV_LCD_BUF_TOP     1   /* 顶层缓冲区（通常用于叠加层绘制） */

/* LCD最大尺寸（编译期常量，决定内部缓冲区大小） */
#define DEV_LCD_MAX_WIDTH   128  /* 最大列数（像素） */
#define DEV_LCD_MAX_HEIGHT  64   /* 最大行数（像素） */
#define DEV_LCD_MAX_PAGES   (DEV_LCD_MAX_HEIGHT / 8)  /* 最大页数 = 高度/8 */

/**
 * @brief 脏区域标记结构体
 *
 * 记录某个page中被修改过的列范围。
 * start_col > end_col 表示该page未被修改（无脏数据）。
 */
typedef struct {
    uint8_t start_col;  /* 脏区域起始列（含） */
    uint8_t end_col;    /* 脏区域结束列（含） */
} dev_lcd_dirty_t;

/**
 * @brief LCD设备结构体
 *
 * 包含硬件配置和控制器操作函数指针。
 * 用户在应用层定义此结构体并填充具体硬件参数后，调用dev_lcd_init()即可使用。
 *
 * 3-wire SPI + DC接线说明：
 *   SCK  → SPI时钟
 *   SDA  → SPI数据（MOSI）
 *   CS   → 片选（低有效，每个LCD独占）
 *   DC   → 数据/命令选择（低=命令，高=数据）
 *   RST  → 复位（低有效，可选，port=0xFFFFFFFF表示不使用）
 */
typedef struct dev_lcd_dev {
    /* ---- SPI总线（可多个设备共享） ---- */
    bsp_spi_soft_dev_t *bus;

    /* ---- CS 引脚（片选，低有效，每个LCD独占一个CS） ---- */
    uint32_t cs_port;
    uint32_t cs_pin;

    /* ---- DC 引脚（数据/命令选择：低电平=命令字，高电平=数据） ---- */
    uint32_t dc_port;
    uint32_t dc_pin;

    /* ---- RST 引脚（复位，低有效，port=0xFFFFFFFF 表示不使用硬件复位） ---- */
    uint32_t rst_port;
    uint32_t rst_pin;

    /* ---- GPIO 操作函数指针（平台相关，由应用层提供） ---- */
    void    (*gpio_set_high)(uint32_t port, uint32_t pin);
    void    (*gpio_set_low)(uint32_t port, uint32_t pin);
    void    (*gpio_set_output)(uint32_t port, uint32_t pin);

    /* ---- 硬件操作函数指针（由具体LCD驱动实现，如SSD1306/ST7565） ---- */
    int     (*hw_init)(struct dev_lcd_dev *dev);         /* 初始化LCD控制器（发送初始化序列） */
    void    (*hw_deinit)(struct dev_lcd_dev *dev);       /* 关闭LCD显示 */
    void    (*hw_write_region)(struct dev_lcd_dev *dev,  /* 向指定page的指定列范围写入数据 */
                               uint8_t page, uint8_t start_col,
                               const uint8_t *data, uint16_t len);

    /* ---- LCD实际尺寸（运行时配置，不超过MAX宏） ---- */
    uint8_t width;   /* 实际列数（像素），如128 */
    uint8_t height;  /* 实际行数（像素），如64 */
    uint8_t pages;   /* 实际页数 = height / 8，如8 */
} dev_lcd_dev_t;

/* ########################### 对外接口 ########################### */

/**
 * @brief 初始化LCD控制器
 * @param dev  LCD设备结构体指针（需预先填充硬件参数和hw_init函数）
 * @return 0=成功, -1=失败
 */
int     dev_lcd_init(dev_lcd_dev_t *dev);

/**
 * @brief 关闭LCD显示
 * @param dev  LCD设备结构体指针
 */
void    dev_lcd_deinit(dev_lcd_dev_t *dev);

/**
 * @brief 设置单个像素
 * @param dev    LCD设备结构体指针
 * @param index  缓冲区索引（DEV_LCD_BUF_BASE 或 DEV_LCD_BUF_TOP）
 * @param x      列坐标（0 ~ width-1）
 * @param y      行坐标（0 ~ height-1）
 * @param color  像素颜色（DEV_LCD_BLACK 或 DEV_LCD_WHITE）
 * @return 0=成功, 1=参数越界
 */
uint8_t dev_lcd_set_pixel(dev_lcd_dev_t *dev, uint8_t index,
                          uint8_t x, uint8_t y, uint8_t color);

/**
 * @brief 设置单列单页像素（一个字节，8个垂直像素）
 * @param dev    LCD设备结构体指针
 * @param index  缓冲区索引
 * @param x      列坐标
 * @param y      起始行坐标（y ~ y+h-1 范围内的像素被设置）
 * @param h      像素高度（1~8），跨页时自动拆分为两个page操作
 * @param data   像素数据（高位=起始行）
 * @return 0=成功, 1=参数越界
 */
uint8_t dev_lcd_set_page_pixel(dev_lcd_dev_t *dev, uint8_t index,
                               uint8_t x, uint8_t y, uint8_t h, uint8_t data);

/**
 * @brief 批量设置多列单页像素（逐列写入不同数据，适用于图像/字库渲染）
 * @param dev    LCD设备结构体指针
 * @param index  缓冲区索引
 * @param x      起始列坐标
 * @param y      起始行坐标
 * @param h      像素高度（1~8），跨页时自动拆分
 * @param w      列数（像素宽度）
 * @param data   像素数据数组（每列1字节，共w字节）
 * @return 0=成功, 1=参数越界
 */
uint8_t dev_lcd_set_mult_page_pixel(dev_lcd_dev_t *dev, uint8_t index,
                                     uint8_t x, uint8_t y, uint8_t h,
                                     uint16_t w, const uint8_t *data);

/**
 * @brief 批量填充多列单页像素（所有列写入相同数据，适用于清屏/画矩形）
 * @param dev    LCD设备结构体指针
 * @param index  缓冲区索引
 * @param x      起始列坐标
 * @param y      起始行坐标
 * @param h      像素高度（1~8），跨页时自动拆分
 * @param w      列数（像素宽度）
 * @param data   统一的像素数据（所有列填充相同值）
 * @return 0=成功, 1=参数越界
 */
uint8_t dev_lcd_set_mult_same_page_pixel(dev_lcd_dev_t *dev, uint8_t index,
                                          uint8_t x, uint8_t y, uint8_t h,
                                          uint16_t w, uint8_t data);

/**
 * @brief 将缓冲区脏区域刷新到LCD屏幕
 * @param dev    LCD设备结构体指针
 * @param index  缓冲区索引
 *
 * 遍历所有page，仅将被修改过的列范围通过hw_write_region()发送到LCD控制器。
 * 刷新后自动清除脏标记。
 */
void    dev_lcd_refresh(dev_lcd_dev_t *dev, uint8_t index);

/**
 * @brief 清空缓冲区（全白）并标记所有区域为脏
 * @param dev    LCD设备结构体指针
 * @param index  缓冲区索引
 *
 * 清空后需调用dev_lcd_refresh()才能在屏幕上显示效果。
 */
void    dev_lcd_clear(dev_lcd_dev_t *dev, uint8_t index);

/* ########################### 使用示例 ########################### */

/*
 * // 1. 定义LCD控制器操作函数（以SSD1306为例，实际项目中放在单独的驱动文件）
 *
 * // 发送命令字（DC=低电平）
 * static void ssd1306_write_cmd(dev_lcd_dev_t *dev, uint8_t cmd)
 * {
 *     dev->gpio_set_low(dev->cs_port, dev->cs_pin);   // 拉低CS，选中LCD
 *     dev->gpio_set_low(dev->dc_port, dev->dc_pin);    // DC=0，表示命令
 *     bsp_spi_soft_transfer(dev->bus, cmd);             // 通过SPI发送命令字节
 *     dev->gpio_set_high(dev->cs_port, dev->cs_pin);   // 拉高CS，释放LCD
 * }
 *
 * // 发送数据（DC=高电平）
 * static void ssd1306_write_data(dev_lcd_dev_t *dev, const uint8_t *data, uint16_t len)
 * {
 *     dev->gpio_set_low(dev->cs_port, dev->cs_pin);   // 拉低CS，选中LCD
 *     dev->gpio_set_high(dev->dc_port, dev->dc_pin);   // DC=1，表示数据
 *     for (uint16_t i = 0; i < len; i++)
 *         bsp_spi_soft_transfer(dev->bus, data[i]);    // 逐字节发送像素数据
 *     dev->gpio_set_high(dev->cs_port, dev->cs_pin);   // 拉高CS，释放LCD
 * }
 *
 * // LCD硬件初始化（配置GPIO方向、SPI总线、发送SSD1306初始化序列）
 * static int my_hw_init(dev_lcd_dev_t *dev)
 * {
 *     dev->gpio_set_output(dev->cs_port, dev->cs_pin);  // CS配置为输出
 *     dev->gpio_set_output(dev->dc_port, dev->dc_pin);  // DC配置为输出
 *     dev->gpio_set_high(dev->cs_port, dev->cs_pin);    // CS默认高电平（未选中）
 *     bsp_spi_soft_init(dev->bus);                       // 初始化SPI总线
 *     ssd1306_write_cmd(dev, 0xAF);                      // SSD1306: DISPLAY_ON
 *     return 0;
 * }
 *
 * // LCD硬件关闭（发送关闭显示命令）
 * static void my_hw_deinit(dev_lcd_dev_t *dev)
 * {
 *     ssd1306_write_cmd(dev, 0xAE);  // SSD1306: DISPLAY_OFF
 * }
 *
 * // 向指定page的指定列范围写入像素数据
 * static void my_hw_write_region(dev_lcd_dev_t *dev, uint8_t page,
 *                                 uint8_t start_col, const uint8_t *data, uint16_t len)
 * {
 *     ssd1306_write_cmd(dev, 0xB0 | page);                  // 设置页地址
 *     ssd1306_write_cmd(dev, 0x00 | (start_col & 0x0F));    // 设置列地址低4位
 *     ssd1306_write_cmd(dev, 0x10 | ((start_col >> 4) & 0x0F)); // 设置列地址高4位
 *     ssd1306_write_data(dev, data, len);                   // 写入像素数据
 * }
 *
 * // 2. 定义设备结构体（填充硬件参数和函数指针）
 * dev_lcd_dev_t lcd = {
 *     .bus       = &spi_bus,                    // 共享的SPI总线
 *     .cs_port   = GPIOB, .cs_pin  = GPIO_PIN_0,  // CS引脚
 *     .dc_port   = GPIOB, .dc_pin  = GPIO_PIN_1,  // DC引脚
 *     .rst_port  = GPIOB, .rst_pin = GPIO_PIN_2,  // RST引脚（可选）
 *     .gpio_set_high   = gpio_set_high,         // GPIO平台函数
 *     .gpio_set_low    = gpio_set_low,
 *     .gpio_set_output = gpio_set_output,
 *     .hw_init         = my_hw_init,            // LCD控制器初始化函数
 *     .hw_deinit       = my_hw_deinit,          // LCD控制器关闭函数
 *     .hw_write_region = my_hw_write_region,    // LCD数据写入函数
 *     .width  = 128,                            // LCD实际宽度（像素）
 *     .height = 64,                             // LCD实际高度（像素）
 *     .pages  = 8,                              // 页数 = 64/8
 * };
 *
 * // 3. 使用（所有绘图操作通过dev_lcd_*接口，与硬件无关）
 * dev_lcd_init(&lcd);                                      // 初始化LCD
 * dev_lcd_clear(&lcd, DEV_LCD_BUF_BASE);                   // 清空缓冲区（全白）
 * dev_lcd_set_pixel(&lcd, DEV_LCD_BUF_BASE, 10, 20, 1);   // 画一个黑色像素点(10,20)
 * dev_lcd_set_pixel(&lcd, DEV_LCD_BUF_BASE, 11, 20, 1);   // 画一个黑色像素点(11,20)
 * dev_lcd_refresh(&lcd, DEV_LCD_BUF_BASE);                 // 刷新到屏幕
 */

#endif /* __DEV_LCD_H */
