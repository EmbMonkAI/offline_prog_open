#ifndef __BSP_GPIO_H
#define __BSP_GPIO_H

#include <stdint.h>

/**
 * @file bsp_gpio.h
 * @brief 通用 GPIO 板级接口（STM32 HAL 实现）
 *
 * 为 device 层（dev_led / dev_lcd / ...）提供"字面电平"的 GPIO 操作，
 * 注入到各 dev_xxx_dev_t 的 gpio_set_high/gpio_set_low/gpio_set_output 函数指针。
 * 设备的极性/语义由各 dev_xxx 自行处理，本层只做纯 GPIO，可被所有设备复用。
 *
 * pin 类型用 uint32_t 以对齐 dev_xxx_dev_t 的函数指针签名。
 * port 传 GPIO 端口基址（如 (uint32_t)GPIOC），pin 传 GPIO_PIN_x。
 *
 * 注：modules/bsp/bsp_gpio.h 也声明了同名接口但 pin 为 uint16_t，
 *     属框架既有不一致；本工程按 uint32_t 实现，不依赖 modules 那份。
 */

void    bsp_gpio_set_high(uint32_t port, uint32_t pin);    /* 输出高电平 */
void    bsp_gpio_set_low(uint32_t port, uint32_t pin);     /* 输出低电平 */
void    bsp_gpio_set_output(uint32_t port, uint32_t pin);  /* 配置为推挽输出 */
void    bsp_gpio_set_input(uint32_t port, uint32_t pin);      /* 配置为输入(无上下拉) */
void    bsp_gpio_set_input_pullup(uint32_t port, uint32_t pin); /* 配置为上拉输入 */
uint8_t bsp_gpio_get_level(uint32_t port, uint32_t pin);   /* 读电平: 1=高, 0=低 */

#endif /* __BSP_GPIO_H */
