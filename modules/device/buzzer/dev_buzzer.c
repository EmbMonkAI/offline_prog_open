/**
 * @brief 蜂鸣器设备实现（硬件无关，通用）
 *
 * 按 drive 方式分发：
 *   PWM  模式 -> 注入的 hw_on/hw_off（按 freq/duty 启停）。
 *   电平模式 -> 注入的 gpio_set_high/gpio_set_low，并按 active_low 决定有效电平
 *               （与 dev_led 同规则：on ^ active_low 为真 -> 输出高）。
 */

#include "dev_buzzer.h"

/* ==================== 内部辅助 ==================== */

/* 电平驱动：按极性输出有效(on=鸣响)/无效(off=静音)电平。 */
static void dev_buzzer_apply_level(dev_buzzer_dev_t *dev, uint8_t on)
{
    if (on ^ dev->active_low)
        dev->gpio_set_high(dev->port, dev->pin);
    else
        dev->gpio_set_low(dev->port, dev->pin);
}

/* PWM 驱动：参数有效(freq&&duty)则鸣响，否则静音。 */
static void dev_buzzer_apply_pwm(dev_buzzer_dev_t *dev)
{
    if (dev->freq != 0 && dev->duty != 0)
        dev->hw_on(dev);
    else
        dev->hw_off(dev);
}

/* ==================== 对外接口实现 ==================== */

void dev_buzzer_init(dev_buzzer_dev_t *dev)
{
    if (!dev) return;

    if (dev->drive == DEV_BUZZER_DRIVE_LEVEL)
        dev_buzzer_apply_level(dev, 0);     /* 默认静音 */
    else
        dev->hw_off(dev);                   /* PWM 默认停止 */

    dev->state = DEV_BUZZER_OFF;
}

void dev_buzzer_on(dev_buzzer_dev_t *dev)
{
    if (!dev) return;

    if (dev->drive == DEV_BUZZER_DRIVE_LEVEL) {
        dev_buzzer_apply_level(dev, 1);
        dev->state = DEV_BUZZER_ON;
    } else {
        dev_buzzer_apply_pwm(dev);
        dev->state = (dev->freq != 0 && dev->duty != 0) ? DEV_BUZZER_ON : DEV_BUZZER_OFF;
    }
}

void dev_buzzer_off(dev_buzzer_dev_t *dev)
{
    if (!dev) return;

    if (dev->drive == DEV_BUZZER_DRIVE_LEVEL)
        dev_buzzer_apply_level(dev, 0);
    else
        dev->hw_off(dev);

    dev->state = DEV_BUZZER_OFF;
}

void dev_buzzer_set_freq(dev_buzzer_dev_t *dev, uint32_t freq)
{
    if (!dev) return;

    dev->freq = freq;
    /* 仅 PWM 模式且正在鸣响时实时变调 */
    if (dev->drive == DEV_BUZZER_DRIVE_PWM && dev->state == DEV_BUZZER_ON)
        dev_buzzer_apply_pwm(dev);
}

void dev_buzzer_set_duty(dev_buzzer_dev_t *dev, uint8_t duty)
{
    if (!dev) return;

    if (duty > DEV_BUZZER_DUTY_MAX) duty = DEV_BUZZER_DUTY_MAX;
    dev->duty = duty;
    if (dev->drive == DEV_BUZZER_DRIVE_PWM && dev->state == DEV_BUZZER_ON)
        dev_buzzer_apply_pwm(dev);
}

void dev_buzzer_beep(dev_buzzer_dev_t *dev, uint32_t freq, uint8_t duty)
{
    if (!dev) return;

    if (duty > DEV_BUZZER_DUTY_MAX) duty = DEV_BUZZER_DUTY_MAX;
    dev->freq = freq;
    dev->duty = duty;
    dev_buzzer_on(dev);   /* PWM: 按 freq/duty 鸣响；电平: 直接鸣响(freq/duty 被忽略) */
}

uint8_t dev_buzzer_get_state(dev_buzzer_dev_t *dev)
{
    if (!dev) return DEV_BUZZER_OFF;
    return dev->state;
}
