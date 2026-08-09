#ifndef __DEV_LCD_TFT_H
#define __DEV_LCD_TFT_H

#include <stdint.h>

/**
 * @brief TFT LCD 显示模块稳定层（硬件无关）
 *
 * dev_lcd_tft_dev_t 包含硬件配置（SPI、GPIO）和 LCD 控制器操作函数指针。
 * 像素缓冲区由模块内部管理，不对外暴露。
 *
 * SPI 总线以"回调"注入而非绑定具体实现（如 bsp_spi_soft）：真屏可注入
 * 硬件 SPI（HAL_SPI_Transmit）或软件 SPI，模拟后端全部置 NULL。
 *
 * 与单色 dev_lcd 的区别：
 *   - 像素模型为 RGB565（每像素 2 字节），非 1-bit 页寻址
 *   - 刷新原语为"写入事务"（begin_write + write_pixels + end_write，
 *     事务内 CS 保持选中），适配 ST7789/ILI9341 等 TFT
 *   - 无全屏帧缓冲（240×240×2=115KB 放不进 MCU SRAM）：set_pixel 一边画一边
 *     攒"行内 run"批量推屏，详见 dev_lcd_tft.c 顶部说明
 *
 * 更换 TFT 硬件时（如 ST7789 → ILI9341/ST7735），只需重新实现
 * hw_init / hw_set_window / hw_write_pixels 三个回调，稳定层和绘图层无需改动。
 * PC 模拟时，用 sim_lcd_tft.c 链接替换真屏后端即可（实现同一组 hw 回调）。
 *
 * 像素字节序：
 *   发送时按"大端"（高字节在前）拆分，匹配 ST7789 的 SPI 像素顺序。
 *
 * run 流水（无帧缓冲的攒批）：
 *   set_pixel 连续调用（相邻坐标）自动攒成一段 run，断续或行缓冲满时推屏；
 *   refresh() 只做收尾。get_pixel 需后端提供 hw_read_pixel（可选；真屏无 MISO
 *   只写不读，返回 0）。
 */

/* ==================== 像素颜色（RGB565） ==================== */

typedef uint16_t dev_lcd_tft_color_t;   /* RGB565：高5红 / 中6绿 / 低5蓝 */

#define DEV_LCD_TFT_RGB565(r, g, b) \
    ((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | (((uint16_t)(b) & 0xF8) >> 3))

#define DEV_LCD_TFT_BLACK   0x0000
#define DEV_LCD_TFT_WHITE   0xFFFF
#define DEV_LCD_TFT_RED     0xF800
#define DEV_LCD_TFT_GREEN   0x07E0
#define DEV_LCD_TFT_BLUE    0x001F
#define DEV_LCD_TFT_YELLOW  0xFFE0
#define DEV_LCD_TFT_CYAN    0x07FF
#define DEV_LCD_TFT_MAGENTA 0xF81F
#define DEV_LCD_TFT_GRAY    0x8410   /* 0x5F5F5F → RGB565 约 0x8410 */

/* ==================== LCD 物理尺寸（编译期常量） ==================== */

#define DEV_LCD_TFT_WIDTH   240   /* 列数（像素），pcb010-V2.0 实装 1.54" 240×240 模组 */
#define DEV_LCD_TFT_HEIGHT  240   /* 行数（像素） */

/* ==================== 设备结构体 ==================== */

/**
 * @brief TFT LCD 设备结构体
 *
 * 包含硬件配置和控制器操作函数指针。
 * 用户在应用层（或模拟后端）填充此结构体后调用 dev_lcd_tft_init() 即可使用。
 *
 * 4-wire SPI 接线说明：
 *   SCK  → SPI 时钟
 *   SDA  → SPI 数据（MOSI）
 *   CS   → 片选（低有效，每个 LCD 独占）
 *   DC   → 数据/命令选择（低=命令，高=数据）
 *   RST  → 复位（低有效，可选，port=0xFFFFFFFF 表示不使用）
 *   BL   → 背光（高有效，可选，port=0xFFFFFFFF 表示不使用）
 */
typedef struct dev_lcd_tft_dev {
    /* ---- SPI 总线回调（真屏后端注入：硬件 SPI 或软件 SPI；模拟后端置 NULL） ---- */
    void    (*spi_init)(struct dev_lcd_tft_dev *dev);                /* 总线初始化（时钟/引脚） */
    void    (*spi_write_byte)(struct dev_lcd_tft_dev *dev, uint8_t b);           /* 发送 1 字节 */
    void    (*spi_write_buf)(struct dev_lcd_tft_dev *dev,                         /* 批量发送 */
                             const uint8_t *data, uint32_t len);

    /* ---- CS 引脚（片选，低有效） ---- */
    uint32_t cs_port;
    uint32_t cs_pin;

    /* ---- DC 引脚（数据/命令选择：低=命令，高=数据） ---- */
    uint32_t dc_port;
    uint32_t dc_pin;

    /* ---- RST 引脚（复位，低有效；port=0xFFFFFFFF 表示不使用） ---- */
    uint32_t rst_port;
    uint32_t rst_pin;

    /* ---- BL 引脚（背光，高有效；port=0xFFFFFFFF 表示不使用） ---- */
    uint32_t bl_port;
    uint32_t bl_pin;

    /* ---- GPIO 操作函数指针（平台相关，由应用层提供） ---- */
    void    (*gpio_set_high)(uint32_t port, uint32_t pin);
    void    (*gpio_set_low)(uint32_t port, uint32_t pin);
    void    (*gpio_set_output)(uint32_t port, uint32_t pin);

    /* ---- 毫秒延时函数指针（初始化序列需要；由应用层提供） ---- */
    void    (*delay_ms)(uint32_t ms);

    /* ---- 微秒延时函数指针（软件 SPI 时序需要；硬件 SPI 可置 NULL） ---- */
    void    (*delay_us)(uint32_t us);

    /* ---- 硬件操作回调（由具体 TFT 驱动实现，如 ST7789；模拟后端用 SDL 实现） ----
     * 写入事务三段式：begin(设窗口+开CS) → write_pixels(可多次) → end(收CS)。
     * 一次事务内 CS 保持低电平 —— ST7789 命令与参数间被 CS 打断会导致
     * 状态机错乱（参数被当命令吞掉、窗口错位 → 花屏，实测见 DEVELOPMENT.md §13.12）。
     */
    int     (*hw_init)(struct dev_lcd_tft_dev *dev);                       /* 初始化 LCD 控制器 */
    void    (*hw_begin_write)(struct dev_lcd_tft_dev *dev,                 /* 开写事务：设窗口 + CS↓ */
                              uint16_t x0, uint16_t y0,
                              uint16_t x1, uint16_t y1);
    void    (*hw_write_pixels)(struct dev_lcd_tft_dev *dev,                /* 事务内写像素（可多次调） */
                               const uint8_t *data, uint32_t len);
    void    (*hw_end_write)(struct dev_lcd_tft_dev *dev);                  /* 结束事务：CS↑ */
    /* 可选：像素读回（模拟后端读 shadow_buf；真屏 ST7789 无 MISO，不提供则 get_pixel 返回 0） */
    dev_lcd_tft_color_t (*hw_read_pixel)(struct dev_lcd_tft_dev *dev, uint16_t x, uint16_t y);
} dev_lcd_tft_dev_t;

/* ==================== 对外接口 ==================== */

/**
 * @brief 初始化 TFT LCD 控制器
 * @param dev  设备结构体指针（需预先填充硬件参数和 hw_init 回调）
 * @return 0=成功, -1=失败
 */
int     dev_lcd_tft_init(dev_lcd_tft_dev_t *dev);

/**
 * @brief 设置单个像素（直接渲染：攒进行内 run，断续/满时自动推屏）
 * @param dev    设备结构体指针
 * @param x      列坐标（0 ~ WIDTH-1）
 * @param y      行坐标（0 ~ HEIGHT-1）
 * @param color  像素颜色（RGB565）
 * @return 0=成功, 1=参数越界
 */
uint8_t dev_lcd_tft_set_pixel(dev_lcd_tft_dev_t *dev,
                              uint16_t x, uint16_t y, dev_lcd_tft_color_t color);

/**
 * @brief 读取单个像素（需后端 hw_read_pixel；未提供返回 0）
 * @param dev  设备结构体指针
 * @param x    列坐标
 * @param y    行坐标
 * @return 像素颜色（RGB565）；越界或后端不支持读返回 0
 */
dev_lcd_tft_color_t dev_lcd_tft_get_pixel(dev_lcd_tft_dev_t *dev,
                                          uint16_t x, uint16_t y);

/**
 * @brief 全屏清屏（直接把整屏填充为指定颜色并推屏）
 * @param dev    设备结构体指针
 * @param color  填充颜色
 */
void    dev_lcd_tft_clear(dev_lcd_tft_dev_t *dev, dev_lcd_tft_color_t color);

/**
 * @brief 刷新收尾：把流水里最后一个未发送的 run 推到 LCD
 * @param dev  设备结构体指针
 *
 * 直接渲染模型下多数数据已随 set_pixel 上屏，本函数只保证流水排空。
 * 保留该接口以维持"画完统一 refresh"的调用习惯。
 */
void    dev_lcd_tft_refresh(dev_lcd_tft_dev_t *dev);

#endif /* __DEV_LCD_TFT_H */
