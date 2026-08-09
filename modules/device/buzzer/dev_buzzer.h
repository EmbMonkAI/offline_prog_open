#ifndef __DEV_BUZZER_H
#define __DEV_BUZZER_H

#include <stdint.h>

/**
 * @brief 蜂鸣器设备对外接口（硬件无关，通用）
 *
 * 统一抽象两种驱动方式的蜂鸣器：
 *   - PWM 驱动（无源蜂鸣器）：频率=音调，占空比≈音量，
 *     由平台注入的 hw_on/hw_off 启停 PWM。
 *   - 电平驱动（有源蜂鸣器 / 外置驱动电路）：高或低电平启停，
 *     极性用 active_low 配置，由平台注入的 gpio_set_high/gpio_set_low 按极性输出
 *     （与 dev_led 同一套极性规则）。
 *
 * 设备层维护逻辑参数与开关状态，具体硬件操作由应用层注入。
 */

/* 蜂鸣器状态 */
#define DEV_BUZZER_OFF  0
#define DEV_BUZZER_ON   1

/* 驱动方式 */
#define DEV_BUZZER_DRIVE_PWM    0   /* PWM 驱动（无源） */
#define DEV_BUZZER_DRIVE_LEVEL  1   /* 电平驱动（有源 / 驱动电路） */

/* 电平驱动有效极性（鸣响时引脚电平） */
#define DEV_BUZZER_ACTIVE_HIGH  0   /* 高电平鸣响 */
#define DEV_BUZZER_ACTIVE_LOW   1   /* 低电平鸣响 */

/* 占空比范围（百分比，PWM 模式） */
#define DEV_BUZZER_DUTY_MAX  100

/**
 * @brief 蜂鸣器设备结构体
 *
 * 用户在应用层定义并按驱动方式填充相应字段后，调用 dev_buzzer_init() 即可使用。
 *   PWM  模式：填 freq/duty + hw_on/hw_off。
 *   电平模式：填 port/pin/active_low + gpio_set_high/gpio_set_low。
 */
typedef struct dev_buzzer_dev {
    /* ---- 驱动方式 ---- */
    uint8_t drive;       /* DEV_BUZZER_DRIVE_PWM 或 DEV_BUZZER_DRIVE_LEVEL */

    /* ---- PWM 驱动参数（drive==PWM 时有效）---- */
    uint32_t freq;       /* 频率 Hz（0 视为静音） */
    uint8_t  duty;       /* 占空比百分比 0~100（≈音量） */

    /* ---- 电平驱动参数（drive==LEVEL 时有效）---- */
    uint32_t port;       /* GPIO 端口基址，如 (uint32_t)GPIOC */
    uint32_t pin;        /* GPIO 引脚号，如 GPIO_PIN_x */
    uint8_t  active_low; /* 有效极性：DEV_BUZZER_ACTIVE_LOW=低电平鸣响，否则高电平鸣响 */

    /* ---- 平台操作函数指针（应用层注入）----
     * PWM  模式：提供 hw_on / hw_off，按 dev->freq/duty 启停 PWM。
     * 电平模式：提供 gpio_set_high / gpio_set_low（极性由设备层按 active_low 处理）。 */
    void (*hw_on)(struct dev_buzzer_dev *dev);           /* PWM：按 freq/duty 启动鸣响 */
    void (*hw_off)(struct dev_buzzer_dev *dev);          /* PWM：停止鸣响 */
    void (*gpio_set_high)(uint32_t port, uint32_t pin);  /* 电平：输出高 */
    void (*gpio_set_low)(uint32_t port, uint32_t pin);   /* 电平：输出低 */

    /* ---- 状态（内部维护）---- */
    uint8_t state;       /* DEV_BUZZER_OFF=0 或 DEV_BUZZER_ON=1 */
} dev_buzzer_dev_t;

/* ########################### 对外接口 ########################### */

/**
 * @brief 初始化蜂鸣器（确保静音，状态置为关闭）
 * @param dev  蜂鸣器设备结构体指针
 */
void dev_buzzer_init(dev_buzzer_dev_t *dev);

/**
 * @brief 开始鸣响
 *
 * PWM 模式按当前 freq/duty 鸣响（freq=0 或 duty=0 时不鸣响）；
 * 电平模式输出有效电平。
 * @param dev  蜂鸣器设备结构体指针
 */
void dev_buzzer_on(dev_buzzer_dev_t *dev);

/**
 * @brief 停止鸣响（静音）
 * @param dev  蜂鸣器设备结构体指针
 */
void dev_buzzer_off(dev_buzzer_dev_t *dev);

/**
 * @brief 设置频率（音调）。仅 PWM 模式有意义；若正在鸣响则立即变调。
 * @param dev   蜂鸣器设备结构体指针
 * @param freq  频率 Hz（0=静音）
 */
void dev_buzzer_set_freq(dev_buzzer_dev_t *dev, uint32_t freq);

/**
 * @brief 设置占空比（音量）。仅 PWM 模式有意义；若正在鸣响则立即生效。
 * @param dev   蜂鸣器设备结构体指针
 * @param duty  占空比百分比 0~100
 */
void dev_buzzer_set_duty(dev_buzzer_dev_t *dev, uint8_t duty);

/**
 * @brief 便捷接口：设置频率与占空比并开始鸣响
 *
 * PWM 模式按 freq/duty 鸣响；电平模式忽略 freq/duty，等效于 on()。
 * @param dev   蜂鸣器设备结构体指针
 * @param freq  频率 Hz
 * @param duty  占空比百分比 0~100
 */
void dev_buzzer_beep(dev_buzzer_dev_t *dev, uint32_t freq, uint8_t duty);

/**
 * @brief 获取当前状态
 * @param dev  蜂鸣器设备结构体指针
 * @return DEV_BUZZER_ON=1, DEV_BUZZER_OFF=0
 */
uint8_t dev_buzzer_get_state(dev_buzzer_dev_t *dev);

/* ########################### 使用示例 ########################### */

/*
 * // —— 方式 A：PWM 驱动（无源蜂鸣器，本项目用法）——
 * static void my_pwm_on(dev_buzzer_dev_t *dev)  { pwm_set(dev->freq, dev->duty); pwm_start(); }
 * static void my_pwm_off(dev_buzzer_dev_t *dev) { pwm_stop(); }
 * dev_buzzer_dev_t buzzer = {
 *     .drive = DEV_BUZZER_DRIVE_PWM,
 *     .freq  = 2700, .duty = 50,
 *     .hw_on = my_pwm_on, .hw_off = my_pwm_off,
 * };
 * dev_buzzer_init(&buzzer);
 * dev_buzzer_on(&buzzer);               // 2700Hz/50% 鸣响
 * dev_buzzer_set_freq(&buzzer, 4000);   // 变调
 * dev_buzzer_off(&buzzer);
 *
 * // —— 方式 B：电平驱动（有源蜂鸣器，低电平鸣响）——
 * dev_buzzer_dev_t buzzer2 = {
 *     .drive = DEV_BUZZER_DRIVE_LEVEL,
 *     .port = (uint32_t)GPIOC, .pin = GPIO_PIN_13,
 *     .active_low = DEV_BUZZER_ACTIVE_LOW,
 *     .gpio_set_high = bsp_gpio_set_high,
 *     .gpio_set_low  = bsp_gpio_set_low,
 * };
 * dev_buzzer_init(&buzzer2);
 * dev_buzzer_on(&buzzer2);    // 输出低电平（鸣响）
 * dev_buzzer_off(&buzzer2);   // 输出高电平（静音）
 */

#endif /* __DEV_BUZZER_H */
