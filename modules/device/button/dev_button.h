#ifndef __DEV_BUTTON_H
#define __DEV_BUTTON_H

#include <stdint.h>

/**
 * @file dev_button.h
 * @brief IO 直连按键设备（设备层，raw）
 *
 * 一个 dev_button_dev_t 代表【一个按键】（独占一个 GPIO 引脚），多个按键 = 多个实例
 * （与 dev_led 同样的"一设备一对象"风格）。
 * 直接读电平判断按下/释放。只读原始状态；防抖/事件/长按由 service 层处理（同 dev_keyboard）。
 * 与矩阵键盘 dev_keyboard 平级、互不依赖（矩阵走扫描，本设备走直读）。
 *
 * 使用：
 *   1. 定义 dev_button_dev_t，填引脚、极性、GPIO 函数
 *   2. dev_button_init()  — 配置引脚为输入
 *   3. dev_button_read()  — 返回 1=按下, 0=释放
 */

/**
 * @brief IO 直连按键设备结构体（单个按键）
 *
 * 用户在应用层定义并填充后，调用 dev_button_init() 即可使用。
 */
typedef struct {
    uint32_t port;      /* GPIO 端口基址，如 (uint32_t)GPIOA */
    uint32_t pin;       /* GPIO 引脚号，如 GPIO_PIN_15 */

    /* GPIO 操作函数指针（平台相关，应用层注入）*/
    void    (*gpio_init_input)(uint32_t port, uint32_t pin);  /* 配置为输入(上拉/下拉按极性由应用给) */
    uint8_t (*gpio_read)(uint32_t port, uint32_t pin);        /* 读电平：1=高, 0=低 */

    uint8_t active_low; /* 有效极性：1=低电平按下(常用,接GND+上拉), 0=高电平按下 */
} dev_button_dev_t;

/**
 * @brief 初始化按键（配置引脚为输入）
 * @param dev  按键设备结构体指针
 */
void dev_button_init(dev_button_dev_t *dev);

/**
 * @brief 读取按键按下状态
 * @param dev  按键设备结构体指针
 * @return 1=按下, 0=释放
 */
uint8_t dev_button_read(dev_button_dev_t *dev);

#endif /* __DEV_BUTTON_H */
