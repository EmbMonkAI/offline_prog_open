/**
 * @brief ST7789 TFT 真屏后端实现（device 层）
 *
 * 实现 dev_lcd_tft_dev_t 的三个 hw 回调：
 *   st7789_tft_hw_init         — 复位 + ST7789 初始化命令序列
 *   st7789_tft_hw_set_window   — 0x2A(列地址) + 0x2B(行地址) + 0x2C(写显存)
 *   st7789_tft_hw_write_pixels — 通过 SPI 批量发送像素字节（DC=数据）
 *
 * 设计说明：
 *   - 本文件只实现"ST7789 这颗控制器"的命令细节，不包含帧缓冲/脏矩形逻辑
 *     （那些在 dev_lcd_tft.c 稳定层，与本文件无关）。
 *   - 换用其他 TFT（ILI9341/ST7735）时，另写一个 xxx_tft.c 实现同一组回调即可，
 *     稳定层与绘图层无需改动。
 *   - 初始化命令序列移植自厂家 demo（1.54TFTspi.C 的 TFT_init），
 *     其中 CASET/RASET 的地址偏移按实际模组调整（见下方 ST7789_X/Y_OFFSET 注释）。
 *
 * 4-wire SPI 时序（与单色 dev_lcd 一致）：
 *   命令：CS=低, DC=低,  发 1 字节
 *   数据：CS=低, DC=高,  发 N 字节
 *
 * ⚠️ CS 事务模型（2026-08-16 花屏修复，见 DEVELOPMENT.md §13.12）：
 *   一段完整的"命令+参数/数据"突发期间 CS 保持低，结束才拉高 —— 与
 *   Adafruit 等主流驱动一致。此前的逐字节翻 CS 让命令与参数间被 CS 打断，
 *   参数字节（数值恰与命令码重合时，如底部行号 0xB4/0xEF）被控制器当成
 *   新命令吞掉 → 窗口错位 → 与地址相关的花屏（顶部行幸存、底部全花）。
 *
 * SPI 总线经 dev->spi_init/spi_write_byte/spi_write_buf 回调注入：
 *   硬件 SPI（HAL）或软件 SPI（bsp_spi_soft）由入口文件（test_tft_hw.c 等）
 *   包装成这三个回调，本文件不关心实现。
 */

#include "dev_lcd_tft.h"

/* ==================== ST7789 命令定义 ==================== */

#define ST7789_NOP      0x00
#define ST7789_SWRESET  0x01
#define ST7789_SLPIN    0x10
#define ST7789_SLPOUT   0x11
#define ST7789_NORON    0x13
#define ST7789_INVOFF   0x20
#define ST7789_INVON    0x21
#define ST7789_DISPOFF  0x28
#define ST7789_DISPON   0x29
#define ST7789_CASET    0x2A   /* 列地址设置 */
#define ST7789_RASET    0x2B   /* 行地址设置 */
#define ST7789_RAMWR    0x2C   /* 显存写入 */
#define ST7789_MADCTL   0x36   /* 内存数据访问控制（方向/颜色序） */
#define ST7789_COLMOD   0x3A   /* 像素格式 */

/**
 * 显存地址偏移：
 *   ST7789 的 GRAM 为 240×240。pcb010-V2.0 实装的是 240×240 模组，
 *   占满整个 GRAM，偏移为 0/0。
 *   （若换 135×240 模组：X=40,Y=0；128×128 居中模组：34/34 —— 以实物为准。
 *    现象：图像偏移/四周黑边时调这里。）
 */
#ifndef ST7789_X_OFFSET
#define ST7789_X_OFFSET 0
#endif
#ifndef ST7789_Y_OFFSET
#define ST7789_Y_OFFSET 0
#endif

/* ==================== 底层 SPI 命令/数据辅助（事务内，不动 CS） ==================== */

/**
 * st7789_tft_write_cmd — 发送单条命令（DC=低）。调用方负责 CS 括号。
 */
static void st7789_tft_write_cmd(dev_lcd_tft_dev_t *dev, uint8_t cmd)
{
    dev->gpio_set_low(dev->dc_port, dev->dc_pin);     /* DC=低，命令 */
    dev->spi_write_byte(dev, cmd);
}

/** CS 拉低，开一段事务 */
static void st7789_tft_cs_low(dev_lcd_tft_dev_t *dev)
{
    dev->gpio_set_low(dev->cs_port, dev->cs_pin);
}

/** CS 拉高，关一段事务 */
static void st7789_tft_cs_high(dev_lcd_tft_dev_t *dev)
{
    dev->gpio_set_high(dev->cs_port, dev->cs_pin);
}

/**
 * st7789_tft_write_cmd_params — 单命令 + N 参数，整段一个 CS 事务
 * （初始化序列用：每条命令独立成段，段内命令与参数不被 CS 打断）
 */
static void st7789_tft_write_cmd_params(dev_lcd_tft_dev_t *dev,
                                        uint8_t cmd, const uint8_t *params, uint8_t n)
{
    st7789_tft_cs_low(dev);
    st7789_tft_write_cmd(dev, cmd);
    dev->gpio_set_high(dev->dc_port, dev->dc_pin);    /* DC=高，后续都是参数 */
    for (uint8_t i = 0; i < n; i++)
        dev->spi_write_byte(dev, params[i]);
    st7789_tft_cs_high(dev);
}

/** 事务内发 16 位地址（CASET/RASET 参数），DC 已为数据 */
static void st7789_tft_spi_write4(dev_lcd_tft_dev_t *dev, uint16_t s, uint16_t e)
{
    const uint8_t a[4] = {(uint8_t)(s >> 8), (uint8_t)s, (uint8_t)(e >> 8), (uint8_t)e};
    dev->spi_write_buf(dev, a, 4);
}

/* ==================== hw 回调实现 ==================== */

/**
 * st7789_tft_hw_init — 复位 + ST7789 初始化命令序列
 *
 * 序列移植自厂家 demo TFT_init()，像素格式锁定 16bpp(RGB565, 0x55)。
 * 背光引脚（若接线有）在初始化末尾点亮。
 */
int st7789_tft_hw_init(dev_lcd_tft_dev_t *dev)
{
    if (!dev || !dev->spi_write_byte || !dev->spi_write_buf) return -1;

    /* ---- 硬件复位 ---- */
    if (dev->rst_port != 0xFFFFFFFF) {
        dev->gpio_set_output(dev->rst_port, dev->rst_pin);
        dev->gpio_set_low(dev->rst_port, dev->rst_pin);
        dev->delay_ms(10);
        dev->gpio_set_high(dev->rst_port, dev->rst_pin);
        dev->delay_ms(120);
    }

    /* ---- 背光引脚配置（若接线有，高有效） ---- */
    if (dev->bl_port != 0xFFFFFFFF) {
        dev->gpio_set_output(dev->bl_port, dev->bl_pin);
        dev->gpio_set_high(dev->bl_port, dev->bl_pin);   /* 点亮背光 */
    }

    /* ---- CS/DC 配置为输出，CS 默认高（未选中） ---- */
    dev->gpio_set_output(dev->cs_port, dev->cs_pin);
    dev->gpio_set_output(dev->dc_port, dev->dc_pin);
    dev->gpio_set_high(dev->cs_port, dev->cs_pin);

    if (dev->spi_init)
        dev->spi_init(dev);   /* SPI 总线初始化（硬件 SPI 已由 CubeMX 初始化时可置 NULL） */

    /* ---- Sleep Out（单命令独立事务）---- */
    st7789_tft_cs_low(dev);
    st7789_tft_write_cmd(dev, ST7789_SLPOUT);
    st7789_tft_cs_high(dev);
    dev->delay_ms(120);

    /* ---- 初始化命令序列：每条"命令+参数"一个 CS 事务 ---- */
    {
        static const uint8_t porch[]  = {0x0C, 0x0C, 0x00, 0x33, 0x33}; /* 0xB2 */
        static const uint8_t gamma[]  = {                               /* 0xE0/0xE1 共用 */
            0xD0, 0x0D, 0x14, 0x0B, 0x0B, 0x07, 0x3A, 0x44,
            0x50, 0x08, 0x13, 0x13, 0x2D, 0x32
        };

        st7789_tft_write_cmd_params(dev, 0xB2, porch, 5);              /* Porch control */
        st7789_tft_write_cmd_params(dev, 0xB7, "\x56", 1);             /* Gate control */
        st7789_tft_write_cmd_params(dev, 0xBB, "\x18", 1);             /* VCOMS */
        st7789_tft_write_cmd_params(dev, 0xC0, "\x2C", 1);             /* LCM control */
        st7789_tft_write_cmd_params(dev, 0xC2, "\x01", 1);             /* VDV/VRH enable */
        st7789_tft_write_cmd_params(dev, 0xC3, "\x1F", 1);             /* VRH set */
        st7789_tft_write_cmd_params(dev, 0xC4, "\x20", 1);             /* VDV setting */
        st7789_tft_write_cmd_params(dev, 0xC6, "\x0F", 1);             /* Frame rate 2 */
        st7789_tft_write_cmd_params(dev, 0xD0, "\xA4\xA1", 2);         /* Power control 1 */
        st7789_tft_write_cmd_params(dev, 0xE0, gamma, 14);             /* Positive gamma */
        st7789_tft_write_cmd_params(dev, 0xE1, gamma, 14);             /* Negative gamma */
        st7789_tft_write_cmd_params(dev, ST7789_MADCTL, "\x00", 1);    /* 方向正常,RGB 序 */
        st7789_tft_write_cmd_params(dev, ST7789_COLMOD, "\x55", 1);    /* 16bpp RGB565 */
    }

    /* ---- 显示反转开（厂家 demo 用 INVON；部分模组需要，依实物调整）---- */
    st7789_tft_cs_low(dev);
    st7789_tft_write_cmd(dev, ST7789_INVON);
    st7789_tft_cs_high(dev);

    /* ---- 显示开 ---- */
    st7789_tft_cs_low(dev);
    st7789_tft_write_cmd(dev, ST7789_DISPON);
    st7789_tft_cs_high(dev);

    /* ---- 上电稳定（§13.17：修左上角红区黑横线）----
     * DISPON 后内部充电泵/驱动仍需时间，立即开始大流量绘制会丢行（红区黑线）。
     * 1) 额外延时 120ms；
     * 2) "预热事务"：满屏窗口 + 写 1 个黑色像素 —— 控制器先把 RAMWR 路径走通，
     *    首段正式写入不再是最早的大事务。（此像素位于 0,0，会被画面覆盖）*/
    dev->delay_ms(120);
    {
        const uint8_t dummy[2] = {0x00, 0x00};
        st7789_tft_cs_low(dev);
        st7789_tft_write_cmd(dev, ST7789_CASET);
        dev->gpio_set_high(dev->dc_port, dev->dc_pin);
        st7789_tft_spi_write4(dev, 0, (uint16_t)(DEV_LCD_TFT_WIDTH - 1));
        st7789_tft_write_cmd(dev, ST7789_RASET);
        dev->gpio_set_high(dev->dc_port, dev->dc_pin);
        st7789_tft_spi_write4(dev, 0, (uint16_t)(DEV_LCD_TFT_HEIGHT - 1));
        st7789_tft_write_cmd(dev, ST7789_RAMWR);
        dev->gpio_set_high(dev->dc_port, dev->dc_pin);
        dev->spi_write_buf(dev, dummy, 2);
        st7789_tft_cs_high(dev);
    }

    return 0;
}

/**
 * st7789_tft_hw_begin_write — 开写事务：设窗口 + RAMWR + CS 保持低
 *
 *   0x2A (CASET)：列地址范围，加 X 偏移
 *   0x2B (RASET)：行地址范围，加 Y 偏移
 *   0x2C (RAMWR)：进入显存写入模式
 *
 * CS 在本函数拉低，直到 hw_end_write 才拉高 —— 窗口命令、RAMWR、像素数据
 * 同属一段事务，中途不被 CS 打断。
 *
 * 注意：窗口地址需加 ST7789_X/Y_OFFSET（模组在 GRAM 中的实际位置），
 *      否则图像偏移/错位。240×240 模组占满 GRAM，偏移 0。
 */
void st7789_tft_hw_begin_write(dev_lcd_tft_dev_t *dev,
                               uint16_t x0, uint16_t y0,
                               uint16_t x1, uint16_t y1)
{
    uint16_t xs = x0 + ST7789_X_OFFSET;
    uint16_t xe = x1 + ST7789_X_OFFSET;
    uint16_t ys = y0 + ST7789_Y_OFFSET;
    uint16_t ye = y1 + ST7789_Y_OFFSET;
    const uint8_t caset[4] = {(uint8_t)(xs >> 8), (uint8_t)xs, (uint8_t)(xe >> 8), (uint8_t)xe};
    const uint8_t raset[4] = {(uint8_t)(ys >> 8), (uint8_t)ys, (uint8_t)(ye >> 8), (uint8_t)ye};

    st7789_tft_cs_low(dev);
    st7789_tft_write_cmd(dev, ST7789_CASET);
    dev->gpio_set_high(dev->dc_port, dev->dc_pin);
    dev->spi_write_buf(dev, caset, 4);
    st7789_tft_write_cmd(dev, ST7789_RASET);
    dev->gpio_set_high(dev->dc_port, dev->dc_pin);
    dev->spi_write_buf(dev, raset, 4);
    st7789_tft_write_cmd(dev, ST7789_RAMWR);
    dev->gpio_set_high(dev->dc_port, dev->dc_pin);   /* 后续 hw_write_pixels 全是数据 */
}

/**
 * st7789_tft_hw_write_pixels — 事务内写像素字节（RGB565 大端）
 *
 * data 已是按行连续的大端 RGB565 字节流；DC 已由 begin_write 置高，直接发。
 */
void st7789_tft_hw_write_pixels(dev_lcd_tft_dev_t *dev,
                                const uint8_t *data, uint32_t len)
{
    dev->spi_write_buf(dev, data, len);
}

/** st7789_tft_hw_end_write — 关写事务：CS 拉高 */
void st7789_tft_hw_end_write(dev_lcd_tft_dev_t *dev)
{
    st7789_tft_cs_high(dev);
}
