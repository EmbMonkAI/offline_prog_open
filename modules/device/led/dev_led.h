#ifndef __DEV_LED_H
#define __DEV_LED_H

#include <stdint.h>

/**
 * @brief LED设备对外接口（硬件无关）
 *
 * dev_led_dev_t 直接包含GPIO引脚和操作函数指针，无需vtable。
 * 更换LED硬件时，只需在应用层重新填充设备结构体即可。
 *
 * 支持功能：
 *   - GPIO直接控制（开/关/翻转）
 *   - 初始化（配置GPIO为输出）
 *   - 极性可配置（active_low：高/低电平点亮）
 */

/* LED状态 */
#define DEV_LED_OFF    0
#define DEV_LED_ON     1

/* LED有效电平（点亮时所需的引脚电平） */
#define DEV_LED_ACTIVE_HIGH  0   /* 高电平点亮 */
#define DEV_LED_ACTIVE_LOW   1   /* 低电平点亮 */

/**
 * @brief LED设备结构体
 *
 * 包含GPIO引脚配置和操作函数指针。
 * 用户在应用层定义此结构体并填充具体硬件参数后，调用dev_led_init()即可使用。
 */
typedef struct {
    /* LED GPIO引脚 */
    uint32_t port;      /* GPIO端口号 */
    uint32_t pin;       /* GPIO引脚号 */

    /* GPIO操作函数指针（平台相关，由应用层提供） */
    void    (*gpio_set_high)(uint32_t port, uint32_t pin);    /* 输出高电平 */
    void    (*gpio_set_low)(uint32_t port, uint32_t pin);     /* 输出低电平 */
    void    (*gpio_set_output)(uint32_t port, uint32_t pin);  /* 配置为输出模式 */

    /* LED有效电平（点亮时引脚电平）：DEV_LED_ACTIVE_LOW=低电平点亮，
     * DEV_LED_ACTIVE_HIGH=高电平点亮(默认，值为0)。 */
    uint8_t active_low;

    /* LED状态（内部维护，初始化后自动更新） */
    uint8_t state;      /* DEV_LED_OFF=0 或 DEV_LED_ON=1 */
} dev_led_dev_t;

/* ########################### 对外接口 ########################### */

/**
 * @brief 初始化LED（配置GPIO为输出，默认关闭）
 * @param dev  LED设备结构体指针
 */
void dev_led_init(dev_led_dev_t *dev);

/**
 * @brief 点亮LED
 * @param dev  LED设备结构体指针
 */
void dev_led_on(dev_led_dev_t *dev);

/**
 * @brief 熄灭LED
 * @param dev  LED设备结构体指针
 */
void dev_led_off(dev_led_dev_t *dev);

/**
 * @brief 翻转LED状态（开→关，关→开）
 * @param dev  LED设备结构体指针
 */
void dev_led_toggle(dev_led_dev_t *dev);

/**
 * @brief 获取LED当前状态
 * @param dev  LED设备结构体指针
 * @return DEV_LED_ON=1, DEV_LED_OFF=0
 */
uint8_t dev_led_get_state(dev_led_dev_t *dev);

/* ########################### 使用示例 ########################### */

/*
 * // 1. 定义LED设备结构体（填充GPIO引脚和平台函数）
 * dev_led_dev_t led = {
 *     .port           = GPIOB,
 *     .pin            = GPIO_PIN_5,
 *     .gpio_set_high  = gpio_set_high,    // 平台GPIO高电平函数
 *     .gpio_set_low   = gpio_set_low,     // 平台GPIO低电平函数
 *     .gpio_set_output = gpio_set_output, // 平台GPIO输出模式函数
 *     .active_low     = 0,                // 0=高电平点亮, 1=低电平点亮
 * };
 *
 * // 2. 初始化（配置为输出，默认关闭）
 * dev_led_init(&led);
 *
 * // 3. 使用
 * dev_led_on(&led);           // 点亮
 * dev_led_off(&led);          // 熄灭
 * dev_led_toggle(&led);       // 翻转
 * uint8_t state = dev_led_get_state(&led);  // 获取当前状态
 */

#endif /* __DEV_LED_H */
