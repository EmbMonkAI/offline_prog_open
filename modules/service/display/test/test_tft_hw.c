/**
 * @file test_tft_hw.c
 * @brief TFT 自检真屏入口（依赖 HAL，仅 Keil 硬件工程编译，不进 PC CMake）
 *
 * 提供 test_tft_run()：填充 TFT 设备实例（硬件 SPI1 + 控制脚）→ 初始化 → 画自检画面。
 * 画图逻辑复用 test_tft.c 的 test_tft_draw_pattern()。
 *
 * 在 app_main_init() 里调用 test_tft_run() 即可。
 *
 * 实际接线（pcb010-V2.0，2026-08 焊接验证）：
 *   SCK = PA5 (SPI1_SCK)   硬件 SPI，CubeMX 已初始化（Mode0, 8bit, MSB, PCLK/2）
 *   SDA = PA7 (SPI1_MOSI)
 *   CS  = PA3              GPIO 输出，低有效
 *   DC  = PB15             GPIO 输出（低=命令，高=数据）
 *   RST = PB14             GPIO 输出，低有效
 *   BL  = 无独立背光脚      bl_port 置 0xFFFFFFFF（部分模组背光常亮/接电源）
 *
 * ⚠️ PA3/PA5/PA7/PB14/PB15 均非 SWD 引脚（SWD=PA13/PA14），调试不受影响。
 */

#include "test_tft.h"
#include "main.h"               /* HAL + hspi1 */
#include "bsp_gpio.h"           /* GPIO 注入 */
#include "svc_display_tft.h"    /* 绘图层（svc_display_tft_init） */

#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "test-tft"
#include "gen_log.h"

/* ==================== 引脚配置（按实际接线改！） ==================== */

#define TFT_CS_PORT    GPIOA
#define TFT_CS_PIN     GPIO_PIN_3
#define TFT_DC_PORT    GPIOB
#define TFT_DC_PIN     GPIO_PIN_15
#define TFT_RST_PORT   GPIOB
#define TFT_RST_PIN    GPIO_PIN_14
#define TFT_BL_PORT    0xFFFFFFFF   /* 无独立背光脚：不使用 */
#define TFT_BL_PIN     0

extern SPI_HandleTypeDef hspi1;   /* CubeMX 生成（main.c） */

/* ==================== 硬件 SPI1 传输后端（注入 dev_lcd_tft_dev_t） ==================== */

/**
 * ST7789 写时序要求 DC 在 CS 拉低前就绪，硬件 SPI 由 HAL_Transmit 完成收发；
 * Mode0/8bit/MSB 与 ST7789 4-wire 写匹配（读 ID 之类的操作本工程不需要）。
 * （v5 诊断实测：整场 10409 次传输 0 错误 —— MCU 侧 SPI 干净，异常在屏/接线侧。）
 */
static void tft_spi_init(dev_lcd_tft_dev_t *dev)
{
    (void)dev;
    /* SPI1 已由 CubeMX 的 MX_SPI1_Init() 初始化（app_main_init 先于本函数执行），无需再动 */
}

static void tft_spi_write_byte(dev_lcd_tft_dev_t *dev, uint8_t b)
{
    (void)dev;
    HAL_SPI_Transmit(&hspi1, &b, 1, 10);
}

static void tft_spi_write_buf(dev_lcd_tft_dev_t *dev, const uint8_t *data, uint32_t len)
{
    (void)dev;
    /* uint32_t → uint16_t：单次最大 480B（一行），无截断风险 */
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, (uint16_t)len, 100);
}

/* ==================== TFT 设备实例（真屏后端） ==================== */

/* ST7789 的 hw 回调，由 st7789_tft.c 提供（三段式写事务） */
extern int  st7789_tft_hw_init(dev_lcd_tft_dev_t *dev);
extern void st7789_tft_hw_begin_write(dev_lcd_tft_dev_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
extern void st7789_tft_hw_write_pixels(dev_lcd_tft_dev_t *dev, const uint8_t *data, uint32_t len);
extern void st7789_tft_hw_end_write(dev_lcd_tft_dev_t *dev);

static dev_lcd_tft_dev_t s_tft = {
    .spi_init      = tft_spi_init,
    .spi_write_byte = tft_spi_write_byte,
    .spi_write_buf = tft_spi_write_buf,

    .cs_port  = (uint32_t)TFT_CS_PORT,  .cs_pin  = TFT_CS_PIN,
    .dc_port  = (uint32_t)TFT_DC_PORT,  .dc_pin  = TFT_DC_PIN,
    .rst_port = (uint32_t)TFT_RST_PORT, .rst_pin = TFT_RST_PIN,
    .bl_port  = TFT_BL_PORT,            .bl_pin  = TFT_BL_PIN,

    .gpio_set_high   = bsp_gpio_set_high,
    .gpio_set_low    = bsp_gpio_set_low,
    .gpio_set_output = bsp_gpio_set_output,
    .delay_ms        = HAL_Delay,

    .hw_init         = st7789_tft_hw_init,
    .hw_begin_write  = st7789_tft_hw_begin_write,
    .hw_write_pixels = st7789_tft_hw_write_pixels,
    .hw_end_write    = st7789_tft_hw_end_write,
};

/* ==================== 对外入口 ==================== */

int test_tft_run(void)
{
    int r;

    /* 版本标记：串口看到此行 = 运行的是事务化 CS 版本（花屏修复，§13.12）。
     * v5 诊断结论：全场 10409 次 SPI 传输 0 错误，MCU 侧干净；
     * 残留左上角少量花屏待查（见 §13.13），诊断固件不要常驻。 */
    gen_log_info("TFT driver v3 (single-CS transaction)\n");

    r = dev_lcd_tft_init(&s_tft);
    if (r != 0) {
        gen_log_err("TFT init FAIL (%d)\n", r);
        return r;
    }
    gen_log_info("TFT init OK\n");

    /* 绘图层绑定 TFT 设备（会初始化字库 dev_font；SD 卡无字库文件时只影响文字，色块照画） */
    r = svc_display_tft_init(&s_tft);
    if (r != 0) {
        gen_log_err("TFT svc init FAIL (%d), pattern only (no font)\n", r);
        /* 不 return：字体失败不阻塞点屏自检（test_tft_draw_pattern 不依赖字体） */
    } else {
        gen_log_info("TFT svc init OK\n");
    }

    /* 画自检画面 → 一次性刷新（单缓冲，不撕裂） */
    test_tft_draw_pattern();
    dev_lcd_tft_refresh(&s_tft);
    gen_log_info("TFT test pattern drawn\n");

    return 0;
}

/* 提供给应用层持续访问的设备指针（可选，后续 UI 模块复用） */
dev_lcd_tft_dev_t *test_tft_get_dev(void)
{
    return &s_tft;
}
