/**
 * @brief LED设备实现（硬件无关）
 *
 * 通过dev_led_dev_t中的GPIO函数指针直接操作LED引脚。
 * 点亮极性由 active_low 决定，本文件不依赖具体电平含义。
 * 无缓冲区、无脏区域，所有操作直接生效。
 */

#include "dev_led.h"

/* ==================== 内部辅助 ==================== */

/* 按极性将LED设为目标状态（on=点亮）。
 * on ^ active_low 为真 -> 输出高电平，否则输出低电平：
 *   active_low=0(高有效)：点亮输出高
 *   active_low=1(低有效)：点亮输出低
 * 同时更新内部状态。*/
static void dev_led_apply(dev_led_dev_t *dev, uint8_t on)
{
    if (on ^ dev->active_low)
        dev->gpio_set_high(dev->port, dev->pin);
    else
        dev->gpio_set_low(dev->port, dev->pin);

    dev->state = on ? DEV_LED_ON : DEV_LED_OFF;
}

/* ==================== 对外接口实现 ==================== */

void dev_led_init(dev_led_dev_t *dev)
{
    if (!dev) return;

    /* 配置GPIO为输出模式 */
    dev->gpio_set_output(dev->port, dev->pin);

    /* 默认关闭LED（按极性输出熄灭电平） */
    dev_led_apply(dev, DEV_LED_OFF);
}

void dev_led_on(dev_led_dev_t *dev)
{
    if (!dev) return;

    dev_led_apply(dev, DEV_LED_ON);
}

void dev_led_off(dev_led_dev_t *dev)
{
    if (!dev) return;

    dev_led_apply(dev, DEV_LED_OFF);
}

void dev_led_toggle(dev_led_dev_t *dev)
{
    if (!dev) return;

    dev_led_apply(dev, dev->state == DEV_LED_ON ? DEV_LED_OFF : DEV_LED_ON);
}

uint8_t dev_led_get_state(dev_led_dev_t *dev)
{
    if (!dev) return DEV_LED_OFF;
    return dev->state;
}
