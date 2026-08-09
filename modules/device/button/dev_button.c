/**
 * @file dev_button.c
 * @brief IO 直连按键设备实现（设备层，raw）
 *
 * 配置引脚为输入，读电平并按极性转换成"按下/释放"。
 * 不含防抖（由 service 层处理）。
 */

#include "dev_button.h"

void dev_button_init(dev_button_dev_t *dev)
{
    if (!dev) return;

    if (dev->gpio_init_input)
        dev->gpio_init_input(dev->port, dev->pin);
}

uint8_t dev_button_read(dev_button_dev_t *dev)
{
    uint8_t level;

    if (!dev || !dev->gpio_read) return 0;

    level = dev->gpio_read(dev->port, dev->pin);   /* 1=高, 0=低 */
    /* active_low: 低电平=按下; 否则高电平=按下 */
    return dev->active_low ? (level == 0u) : (level != 0u);
}
