/**
 * @file bsp_buzzer.c
 * @brief 蜂鸣器板级 PWM 适配实现（STM32 HAL，TIM3_CH1 / PC6）
 *
 * 把 dev_buzzer 的 freq(Hz)/duty(%) 映射到 TIM3_CH1 的 ARR/CCR 并启停 PWM。
 * TIM3 未走 CubeMX（工程 .ioc 无此外设）：本文件自包含初始化 bsp_buzzer_init()，
 * 时钟/引脚（PC6 AF2）/时基（PSC=83 -> 1MHz tick）都在这里完成，app_main 调用一次。
 */

#include "bsp_buzzer.h"
#include "stm32f4xx_hal.h"

static TIM_HandleTypeDef s_htim3;               /* TIM3 本文件私有（不经 CubeMX） */

#define BUZZER_TIM_CHANNEL  TIM_CHANNEL_1       /* PC6 = TIM3_CH1 */
#define BUZZER_TIM_TICK_HZ  1000000u            /* 84MHz / (PSC=83 +1) = 1MHz tick */

void bsp_buzzer_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    TIM_OC_InitTypeDef oc = {0};

    /* ---- 外设与时钟 ---- */
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* ---- 引脚：PC6 复用 TIM3_CH1（AF2） ---- */
    gpio.Pin       = GPIO_PIN_6;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* ---- 时基：1MHz tick，默认周期 ~2.7kHz（ARR 由 pwm_on 按 freq 重算） ---- */
    s_htim3.Instance           = TIM3;
    s_htim3.Init.Prescaler     = 83;
    s_htim3.Init.CounterMode   = TIM_COUNTERMODE_UP;
    s_htim3.Init.Period        = 369;
    s_htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    if (HAL_TIM_PWM_Init(&s_htim3) != HAL_OK) return;

    /* ---- 通道：PWM1，默认 50% 占空（CCR 由 pwm_on 按 duty 重算） ---- */
    oc.OCMode     = TIM_OCMODE_PWM1;
    oc.Pulse      = 185;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(&s_htim3, &oc, BUZZER_TIM_CHANNEL) != HAL_OK) return;
}

void bsp_buzzer_pwm_on(dev_buzzer_dev_t *dev)
{
    uint32_t arr, ccr;

    if (dev->freq == 0) {
        bsp_buzzer_pwm_off(dev);
        return;
    }

    /* ARR = tick / freq - 1（freq Hz 对应的计数周期） */
    arr = BUZZER_TIM_TICK_HZ / dev->freq;
    if (arr == 0) arr = 1;
    arr -= 1;

    /* CCR = (ARR + 1) * duty / 100 */
    ccr = (arr + 1u) * dev->duty / 100u;

    __HAL_TIM_SET_AUTORELOAD(&s_htim3, arr);
    __HAL_TIM_SET_COMPARE(&s_htim3, BUZZER_TIM_CHANNEL, ccr);
    HAL_TIM_PWM_Start(&s_htim3, BUZZER_TIM_CHANNEL);
}

void bsp_buzzer_pwm_off(dev_buzzer_dev_t *dev)
{
    (void)dev;
    HAL_TIM_PWM_Stop(&s_htim3, BUZZER_TIM_CHANNEL);
}
