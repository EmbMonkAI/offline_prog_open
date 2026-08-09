#ifndef __BSP_BUZZER_H
#define __BSP_BUZZER_H

#include "dev_buzzer.h"

/**
 * @file bsp_buzzer.h
 * @brief 蜂鸣器板级 PWM 适配（STM32 HAL，TIM3_CH1 / PC6）
 *
 * 为 dev_buzzer(PWM 模式)提供注入用的 hw_on / hw_off：
 *   - bsp_buzzer_init  : TIM3 + PC6(AF2) 初始化（自包含，不经 CubeMX；上电调一次）。
 *   - bsp_buzzer_pwm_on : 按 dev->freq 重算 ARR、dev->duty 重算 CCR，启动 TIM3_CH1 PWM。
 *   - bsp_buzzer_pwm_off: 停止 TIM3_CH1 PWM。
 */

void bsp_buzzer_init(void);
void bsp_buzzer_pwm_on(dev_buzzer_dev_t *dev);
void bsp_buzzer_pwm_off(dev_buzzer_dev_t *dev);

#endif /* __BSP_BUZZER_H */
