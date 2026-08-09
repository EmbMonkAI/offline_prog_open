/**
 * @file bsp_gpio.c
 * @brief 通用 GPIO 板级实现（STM32 HAL）
 *
 * 为 dev_led / dev_lcd 等设备层提供"字面电平"的 GPIO 操作，注入到设备结构体。
 * 设备极性/语义由各 dev_xxx 自行处理，本层只做纯 GPIO。
 */

#include "bsp_gpio.h"
#include "stm32f4xx_hal.h"

/* ==================== 内部辅助 ==================== */

/* 按端口基址使能对应 GPIO 时钟（幂等；CubeMX 的 MX_GPIO_Init 通常已使能部分）。 */
static void bsp_gpio_clk_enable(uint32_t port)
{
    if      (port == (uint32_t)GPIOA) __HAL_RCC_GPIOA_CLK_ENABLE();
    else if (port == (uint32_t)GPIOB) __HAL_RCC_GPIOB_CLK_ENABLE();
    else if (port == (uint32_t)GPIOC) __HAL_RCC_GPIOC_CLK_ENABLE();
    else if (port == (uint32_t)GPIOD) __HAL_RCC_GPIOD_CLK_ENABLE();
    else if (port == (uint32_t)GPIOE) __HAL_RCC_GPIOE_CLK_ENABLE();
    else if (port == (uint32_t)GPIOH) __HAL_RCC_GPIOH_CLK_ENABLE();
}

/* ==================== 对外接口实现 ==================== */

void bsp_gpio_set_output(uint32_t port, uint32_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    bsp_gpio_clk_enable(port);

    gpio.Pin   = (uint16_t)pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init((GPIO_TypeDef *)port, &gpio);
}

void bsp_gpio_set_input(uint32_t port, uint32_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    bsp_gpio_clk_enable(port);

    gpio.Pin  = (uint16_t)pin;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init((GPIO_TypeDef *)port, &gpio);
}

void bsp_gpio_set_input_pullup(uint32_t port, uint32_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    bsp_gpio_clk_enable(port);

    gpio.Pin  = (uint16_t)pin;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init((GPIO_TypeDef *)port, &gpio);
}

void bsp_gpio_set_high(uint32_t port, uint32_t pin)
{
    HAL_GPIO_WritePin((GPIO_TypeDef *)port, (uint16_t)pin, GPIO_PIN_SET);
}

void bsp_gpio_set_low(uint32_t port, uint32_t pin)
{
    HAL_GPIO_WritePin((GPIO_TypeDef *)port, (uint16_t)pin, GPIO_PIN_RESET);
}

uint8_t bsp_gpio_get_level(uint32_t port, uint32_t pin)
{
    return (HAL_GPIO_ReadPin((GPIO_TypeDef *)port, (uint16_t)pin) == GPIO_PIN_SET) ? 1u : 0u;
}
