/**
 * @file app_main.c
 * @brief 应用层 —— LED + 蜂鸣器 + SDIO + FATFS 调试
 *
 * 分层：app ──调用──▶ dev_led / dev_buzzer(设备层) ──函数指针──▶ bsp(板级)
 *   - LED：红 PC13 / 绿 PC14，低电平点亮（active_low）；循环里作心跳。
 *   - 蜂鸣器：PC6 = TIM3_CH1，PWM 驱动；仅开机"嘀"一声。
 *   - SD：SDIO(4-bit) 卡，HAL_SD 块访问；FATFS 读文件。
 *   - 板级引脚（PCB010-V3.0）：按键 SW_OK=PB14/SW_UP=PA0/SW_DOWN=PB13；
 *     SWD 输出 SWDIO=PB7/SWCLK=PB6/RST=PB5（dap/DAP_config.h）；杂项输出
 *     PC0/PC1/PB1/PB2/PB10/PB12/PB15。
 * 日志：modules/component/gen_log（UART 端口，USART1 PA9/PA10 115200）。测试结果只看串口。
 *
 * 返回值约定：0 = 成功，非0 = 失败码（与 HAL_OK / FR_OK 一致）。
 */

#include "app_main.h"
#include "main.h"          /* CubeMX 宏 + HAL */
#include <string.h>        /* memcmp */
#include "ff.h"            /* FatFs 文件系统（CubeMX 生成） */

#include "dev_led.h"
#include "dev_buzzer.h"
#include "bsp_gpio.h"      /* LED/LCD 注入通用 GPIO */
#include "bsp_buzzer.h"    /* 蜂鸣器 PWM 适配 */
#include "bsp_uart.h"      /* 日志串口输出（USART1） */
#include "dev_button.h"    /* IO 直连按键 */
#include "dev_lcd_tft.h"   /* TFT 设备层 */
#include "svc_display_tft.h" /* 绘图层（svc_display_tft_init） */
#include "test_tft.h"      /* test_tft_draw_pattern（自检画面，真机/PC 共用） */
#include "app_flash.h"     /* 脱机烧录 */
#include "app_usb.h"       /* USB CDC 文件传输 */
#include "app_ui.h"        /* 业务 UI：文件选择 → 参数 → 烧录（app 层） */

/* 日志：套用 modules/component/gen_log（UART 端口，GEN_LOG_PORT_UART） */
#define GEN_LOG_MODULE 1
#define GEN_LOG_TAG    "app"
#include "gen_log.h"

extern SD_HandleTypeDef hsd;   /* CubeMX 生成的 SD 句柄（main.c） */

/* ========== LED（红 PC13 / 绿 PC14，低电平点亮）========== */
static dev_led_dev_t s_led_red = {
    .port            = (uint32_t)LED_R_GPIO_Port,
    .pin             = LED_R_Pin,
    .gpio_set_high   = bsp_gpio_set_high,
    .gpio_set_low    = bsp_gpio_set_low,
    .gpio_set_output = bsp_gpio_set_output,
    .active_low      = DEV_LED_ACTIVE_LOW,
};
static dev_led_dev_t s_led_green = {
    .port            = (uint32_t)LED_G_GPIO_Port,
    .pin             = LED_G_Pin,
    .gpio_set_high   = bsp_gpio_set_high,
    .gpio_set_low    = bsp_gpio_set_low,
    .gpio_set_output = bsp_gpio_set_output,
    .active_low      = DEV_LED_ACTIVE_LOW,
};

/* ========== 蜂鸣器（PWM，PC6 = TIM3_CH1，bsp_buzzer_init 自包含初始化）========== */
static dev_buzzer_dev_t s_buzzer = {
    .drive = DEV_BUZZER_DRIVE_PWM,
    .freq  = 2700,
    .duty  = 50,
    .hw_on  = bsp_buzzer_pwm_on,
    .hw_off = bsp_buzzer_pwm_off,
};

/* ========== 按键（IO 直连，低电平按下：接 GND + 内部上拉；PCB010-V3.0）
 * 初始化统一在本文件 app_main_init()（app_ui 只读实例，不再各自 init）========== */
static dev_button_dev_t s_button = {           /* SW_OK：PB14 */
    .port            = (uint32_t)GPIOB,
    .pin             = GPIO_PIN_14,
    .gpio_init_input = bsp_gpio_set_input_pullup,
    .gpio_read       = bsp_gpio_get_level,
    .active_low      = 1,                      /* 低电平为按下 */
};
static dev_button_dev_t s_btn_up = {           /* SW_UP：PA0 */
    .port            = (uint32_t)GPIOA,
    .pin             = GPIO_PIN_0,
    .gpio_init_input = bsp_gpio_set_input_pullup,
    .gpio_read       = bsp_gpio_get_level,
    .active_low      = 1,
};
static dev_button_dev_t s_btn_down = {         /* SW_DOWN：PB13 */
    .port            = (uint32_t)GPIOB,
    .pin             = GPIO_PIN_13,
    .gpio_init_input = bsp_gpio_set_input_pullup,
    .gpio_read       = bsp_gpio_get_level,
    .active_low      = 1,
};

/* ========== TFT LCD（真机硬件装配）==========
 *
 * 实际接线（PCB010-V3.0）：
 *   SCK = PA5 (SPI1_SCK)   硬件 SPI，CubeMX 已初始化（Mode0, 8bit, MSB, PCLK/2）
 *   SDA = PA7 (SPI1_MOSI)
 *   CS  = PB0              GPIO 输出，低有效
 *   DC  = PC4              GPIO 输出（低=命令，高=数据）
 *   RST = PC5              GPIO 输出，低有效
 *   BL  = PC7              GPIO 输出，高有效（经 R12→Q3 驱动背光，high->ON）
 *
 * ⚠️ PA5/PA7/PB0/PC4/PC5/PC7 均非 SWD 引脚（SWD=PA13/PA14），调试不受影响。*/
#define TFT_CS_PORT    GPIOB
#define TFT_CS_PIN     GPIO_PIN_0
#define TFT_DC_PORT    GPIOC
#define TFT_DC_PIN     GPIO_PIN_4
#define TFT_RST_PORT   GPIOC
#define TFT_RST_PIN    GPIO_PIN_5
#define TFT_BL_PORT    GPIOC
#define TFT_BL_PIN     GPIO_PIN_7

extern SPI_HandleTypeDef hspi1;   /* CubeMX 生成（main.c） */

/* ST7789 写时序要求 DC 在 CS 拉低前就绪，硬件 SPI 由 HAL_Transmit 完成收发；
 * Mode0/8bit/MSB 与 ST7789 4-wire 写匹配（读 ID 之类的操作本工程不需要）。
 * （v5 诊断实测：整场 10409 次 SPI 传输 0 错误 —— MCU 侧 SPI 干净，异常在屏/接线侧。）*/
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
    .bl_port  = (uint32_t)TFT_BL_PORT,  .bl_pin  = TFT_BL_PIN,

    .gpio_set_high   = bsp_gpio_set_high,
    .gpio_set_low    = bsp_gpio_set_low,
    .gpio_set_output = bsp_gpio_set_output,
    .delay_ms        = HAL_Delay,

    .hw_init         = st7789_tft_hw_init,
    .hw_begin_write  = st7789_tft_hw_begin_write,
    .hw_write_pixels = st7789_tft_hw_write_pixels,
    .hw_end_write    = st7789_tft_hw_end_write,
};

/* ========== SDIO 裸块读写自检（已验证 PASS；与 FATFS 共用 SD，暂屏蔽，代码保留）========== */
#if 0
#define SD_TEST_BLK     1024u
#define SD_TEST_TIMEOUT 2000u
static uint32_t sd_orig[128];   /* 512B，4 字节对齐 */
static uint32_t sd_pat[128];
static uint32_t sd_rd[128];

static void sd_wait_ready(void)
{
    uint32_t guard = 0;
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        if (++guard > 1000000u) break;
    }
}

/* 返回: 0=成功; 1=read-orig失败; 2=write失败; 3=read-back失败; 4=数据不一致 */
static int sd_rw_test(void)
{
    uint8_t *orig = (uint8_t *)sd_orig, *pat = (uint8_t *)sd_pat, *rd = (uint8_t *)sd_rd;
    uint32_t i;
    int code = 0, have_orig = 0;

    gen_log_info("SD R/W test @ blk %lu ...\n", (unsigned long)SD_TEST_BLK);
    if (HAL_SD_ReadBlocks(&hsd, orig, SD_TEST_BLK, 1, SD_TEST_TIMEOUT) != HAL_OK) {
        gen_log_err("SD R/W: read-orig FAIL\n");
        code = 1;
    } else {
        have_orig = 1;
        sd_wait_ready();
        for (i = 0; i < 512u; i++) pat[i] = (uint8_t)(i ^ 0xA5);
        if (HAL_SD_WriteBlocks(&hsd, pat, SD_TEST_BLK, 1, SD_TEST_TIMEOUT) != HAL_OK) {
            gen_log_err("SD R/W: write FAIL\n");
            code = 2;
        } else {
            sd_wait_ready();
            if (HAL_SD_ReadBlocks(&hsd, rd, SD_TEST_BLK, 1, SD_TEST_TIMEOUT) != HAL_OK) {
                gen_log_err("SD R/W: read-back FAIL\n");
                code = 3;
            } else {
                sd_wait_ready();
                if (memcmp(pat, rd, 512u) != 0) {
                    gen_log_err("SD R/W: data mismatch\n");
                    code = 4;
                }
            }
        }
    }
    if (have_orig) {   /* 无论成败都恢复原扇区 */
        HAL_SD_WriteBlocks(&hsd, orig, SD_TEST_BLK, 1, SD_TEST_TIMEOUT);
        sd_wait_ready();
    }
    return code;
}
#endif  /* SDIO 裸块读写自检（保留）*/

/* ========== FATFS 文件系统测试 ========== */
static FATFS s_fatfs;        /* 文件系统对象 */
static FIL   s_file;         /* 文件对象（静态，避免占栈） */

/* 返回: 0=成功; 非0=失败码(透传 FatFs FRESULT，如 FR_NO_FILESYSTEM) */
static int fatfs_test(void)
{
    FRESULT fr;
    DIR d;
    FILINFO fi;
    int n;

    fr = f_mount(&s_fatfs, "0:", 1);   /* 1=立即挂载 */
    if (fr != FR_OK) {
        gen_log_err("FATFS: mount FAIL (%d)\n", (int)fr);
        return (int)fr;
    }
    gen_log_info("FATFS: mount OK\n");

    /* 列出根目录文件，验证 FAT 读取（_USE_LFN=2 开启后，fi.fname 即长文件名）*/
    n = 0;
    if (f_opendir(&d, "0:/") == FR_OK) {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0' && n < 8) {
            gen_log_info("  %s%s  %lu B\n", fi.fname,
                         (fi.fattrib & AM_DIR) ? "/" : "",
                         (unsigned long)fi.fsize);
            n++;
        }
        f_closedir(&d);
    }
    gen_log_info("FATFS: root has %d+ entries\n", n);

    /* 若有 test.txt 则读出来，验证文件读取 */
    if (f_open(&s_file, "0:test.txt", FA_READ) == FR_OK) {
        char buf[128];
        UINT br = 0;
        f_read(&s_file, buf, sizeof(buf) - 1, &br);
        buf[br] = '\0';
        gen_log_info("FATFS: test.txt -> %u bytes\n", (unsigned)br);
        f_close(&s_file);
    } else {
        gen_log_info("FATFS: no test.txt (skip)\n");
    }
    return 0;
}

/* 蜂鸣器自检：依次播放几个音调，验证蜂鸣器与变调(set_freq)；返回 0=成功 */
static int buzzer_test(void)
{
    static const uint32_t tones[] = {2700, 3200, 2400};
    uint32_t i;
    for (i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
        dev_buzzer_beep(&s_buzzer, tones[i], 50);
        HAL_Delay(220);
    }
    dev_buzzer_off(&s_buzzer);
    return 0;
}

/* TFT 初始化（设备 + 绘图层 + 自检画面）。返回 0=成功。
 * 原自检入口 test_tft_hw.c/test_tft_run() 已并入本函数（分层：硬件装配归 app 层）。*/
static int tft_setup(void)
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

/* 提供给应用层持续访问的 TFT 设备指针（app_ui 渲染用） */
dev_lcd_tft_dev_t *app_main_get_tft(void)
{
    return &s_tft;
}

/* 按键实例（app_ui 用；初始化见 app_main_init） */
dev_button_dev_t *app_main_get_btn_ok(void)   { return &s_button; }
dev_button_dev_t *app_main_get_btn_up(void)   { return &s_btn_up; }
dev_button_dev_t *app_main_get_btn_down(void) { return &s_btn_down; }

/* 固件版本串（FW_VERSION_STR）——app_usb 的 CMD_GET_VER 查询用 */
const char *app_fw_version_str(void)
{
    return FW_VERSION_STR;
}

void app_main_init(void)
{
    int r;

    /* 日志串口：USART1（PA9/PA10，115200）—— CubeMX 的 MX_USART1_UART_Init 已在
     * main() 里先于本函数初始化，此处无需再动 */

    /* SWD 输出脚预拉高（PB6=SWCLK/PB7=SWDIO/PB5=RST）：MX_GPIO_Init 曾把
     * PB4~PB7 推挽输出写 0（V2 遗留），空闲低电平会让目标 SWD 线挂死。
     * 这里恢复空闲态高电平；烧录时 PORT_SWD_SETUP（DAP_config.h）会再完整配置。*/
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);

    /* 蜂鸣器时基+引脚（TIM3_CH1 / PC6，自包含初始化；随后 dev_buzzer 只启停 PWM）*/
    bsp_buzzer_init();

    /* 杂项 GPIO（PCB010-V3.0）：上拉输出（推挽 + 内部上拉，默认输出高）。
     * 预留扩展位（OUT1~OUT5 等）；PB15=OUT_VCC3V3_CTRL 目标 3.3V 使能上电即通，
     * 若需默认断电改写 GPIO_PIN_RESET。 */
    {
        GPIO_InitTypeDef gi = {0};
        gi.Mode  = GPIO_MODE_OUTPUT_PP;
        gi.Pull  = GPIO_PULLUP;
        gi.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_SET);            /* PC0/PC1（OUT1/OUT2） */
        gi.Pin   = GPIO_PIN_0 | GPIO_PIN_1;
        HAL_GPIO_Init(GPIOC, &gi);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_10              /* PB1/PB2/PB10/PB12/PB15 */
                          | GPIO_PIN_12 | GPIO_PIN_15, GPIO_PIN_SET);
        gi.Pin   = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_10 | GPIO_PIN_12 | GPIO_PIN_15;
        HAL_GPIO_Init(GPIOB, &gi);
    }

    gen_log_info("=== offline_prog boot " FW_VERSION_STR " ===\n");

    /* LED 上电红亮绿灭；蜂鸣器自检（多音调）*/
    dev_led_init(&s_led_red);
    dev_led_init(&s_led_green);
    dev_led_on(&s_led_red);
    dev_led_off(&s_led_green);
    dev_buzzer_init(&s_buzzer);
    r = buzzer_test();
    if (r == 0) gen_log_info("init: buzzer PASS\n");
    else        gen_log_err("init: buzzer FAIL (%d)\n", r);

    /* 按键初始化统一在此（app_ui 只读实例）：SW_OK=PB14 / SW_UP=PA0 / SW_DOWN=PB13 */
    dev_button_init(&s_button);
    dev_button_init(&s_btn_up);
    dev_button_init(&s_btn_down);

    /* SDIO 裸块读写自检（暂屏蔽，见上方 #if 0 段）*/
#if 0
    r = sd_rw_test();
    if (r == 0) gen_log_info("init: SD R/W PASS\n");
    else        gen_log_err("init: SD R/W FAIL (%d)\n", r);
#endif

    /* FATFS 文件系统测试（f_mount 会让 BSP_SD_Init 把卡重新完整初始化）*/
    r = fatfs_test();
    if (r == 0) gen_log_info("init: FATFS PASS\n");
    else        gen_log_err("init: FATFS FAIL (%d)\n", r);

    /* TFT 点屏自检（SPI1 硬件 SPI；字库 kp_font_lib.bin 从 SD 读，故放 FATFS 之后）*/
    r = tft_setup();
    if (r == 0) gen_log_info("init: TFT PASS\n");
    else        gen_log_err("init: TFT FAIL (%d)\n", r);

    /* 业务 UI（app 层）：文件列表 → 按键选择/确认 → 烧录进度/结果 */
    app_ui_init();

    /* USB CDC 文件传输协议层（FATFS 挂载后；CDC 收数据进环，poll 在主循环）*/
    app_usb_init();

    /* SD 卡信息：放在 f_mount 之后读取——此时卡由 BSP_SD_Init 完整初始化，信息才有效 */
    {
        HAL_SD_CardInfoTypeDef ci = {0};
        if (HAL_SD_GetCardInfo(&hsd, &ci) == HAL_OK) {
            gen_log_info("SD: type=%u class=%lu blk=%lu count=%lu (%lu MB)\n",
                         (unsigned)ci.CardType, (unsigned long)ci.Class,
                         (unsigned long)ci.BlockSize, (unsigned long)ci.BlockNbr,
                         (unsigned long)((uint64_t)ci.BlockNbr * ci.BlockSize / (1024U * 1024U)));
        } else {
            gen_log_err("SD: get card info FAIL\n");
        }
    }
}

/* 蜂鸣器响 times 声（每声 120ms，间隔 120ms）—— 烧录结果反馈用 */
static void buzzer_beep_times(int times)
{
    int i;
    for (i = 0; i < times; i++) {
        dev_buzzer_beep(&s_buzzer, 2700, 50);
        HAL_Delay(120);
        dev_buzzer_off(&s_buzzer);
        if (i < times - 1) HAL_Delay(120);
    }
}

/* ========== 烧录结果反馈（LED + 蜂鸣器；app_ui 在烧录结束点调用）==========
 * ok!=0：绿灯亮 + 2.7kHz 短响 1 声
 * ok==0：红灯亮 + 2.7kHz 急促 3 连响（每声 60ms、间隔 40ms）
 * LED 保持到回列表（app_ui 调 app_main_feedback_idle 恢复心跳态）*/
void app_main_burn_feedback(int ok)
{
    int i;

    if (ok) {
        dev_led_on(&s_led_green);
        dev_led_off(&s_led_red);
        dev_buzzer_beep(&s_buzzer, 2700, 50);   /* 短响 1 声 */
        HAL_Delay(120);
        dev_buzzer_off(&s_buzzer);
    } else {
        dev_led_on(&s_led_red);
        dev_led_off(&s_led_green);
        for (i = 0; i < 3; i++) {               /* 急促 3 连响：60ms 响 + 40ms 停 */
            dev_buzzer_beep(&s_buzzer, 2700, 50);
            HAL_Delay(60);
            dev_buzzer_off(&s_buzzer);
            if (i < 2) HAL_Delay(40);
        }
    }
}

/* 恢复待机 LED（红亮绿灭，app_main_init 的上电态；回文件列表时调）*/
void app_main_feedback_idle(void)
{
    dev_led_on(&s_led_red);
    dev_led_off(&s_led_green);
}

void app_main_loop(void)
{
    uint32_t now = HAL_GetTick();
    static uint32_t led_tick = 0;
    (void)now; (void)led_tick;

    /* USB CDC：消化接收环、驱动文件传输协议（无 USB 数据时几乎无开销）*/
    app_usb_poll();

    /* 业务 UI（app 层）：按键状态机 + 页面渲染（烧录在长按确认时同步执行）*/
    app_ui_poll();
}
