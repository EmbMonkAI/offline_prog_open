# 硬件说明 (HARDWARE.md)

> 本板：**脱机烧录器** —— 由 STM32F401RB 独立完成对目标芯片的 SWD 烧录，带 LCD/SD/USB/蜂鸣器/LED。
>
> **当前固件对应 PCB010-V3.0**（2026-08-28 起，固件 V1.5.0）。V2→V3 引脚变更见 §5。
> 引脚映射数据源：`stm32f401_proj/stm32f401_proj.ioc`（CubeMX，仅覆盖其管理的外设）+
> `app/app_main.c`（app 层装配：LCD 控制脚/按键/杂项 GPIO）+ `dap/DAP_config.h`（SWD 输出）+
> `bsp/`（蜂鸣器 TIM3/日志 USART2）。外部硬件细节来源：原理图 `PCB010-V3.0.pdf`。

---

## 1. 主控与时钟

| 项 | 值 |
|----|-----|
| MCU | STM32F401RBT6（LQFP64） |
| 系统时钟 | **84 MHz**（HSE 16MHz × PLL: M=8, N=168, P=/4, Q=7） |
| 总线 | HCLK=84MHz；APB1=42MHz（定时器时钟 84MHz）；APB2=84MHz |
| USB 时钟 | 48MHz（PLLQ） |
| 栈/堆 | Stack 0x1000 / Heap 0x200 |
| 调试 | SWD（PA13/PA14） |

---

## 2. 引脚映射（按功能分组）

### 蜂鸣器（无源，PWM 驱动）
| 引脚 | 信号 | 说明 |
|------|------|------|
| PC6 | TIM3_CH1 | PWM，默认 ~2.7kHz / 50%（PSC=83, ARR 由 bsp_buzzer 按 freq 重算）。TIM3 不走 CubeMX，`bsp_buzzer_init()` 自包含初始化（AF2）。 |

### LED（低电平点亮 active-low）
| 引脚 | CubeMX 标签 | 说明 |
|------|------------|------|
| PC13 | LED_R | 红灯 |
| PC14 | LED_G | 绿灯 ⚠️ 占用 OSC32_IN |

### LCD（SPI1 + 控制脚，TFT ST7789）
| 引脚 | 信号 | 说明 |
|------|------|------|
| PA5 | SPI1_SCK | LCD 时钟（CubeMX） |
| PA6 | SPI1_MISO | 未接（TFT 只写） |
| PA7 | SPI1_MOSI | LCD 数据 |
| PB0 | TFT_CS | 片选，低有效（app_main.c 宏） |
| PC4 | TFT_DC | 数据/命令（低=命令），（app_main.c 宏） |
| PC5 | TFT_RST | 复位，低有效（app_main.c 宏） |
| PC7 | TFT_BL | 背光，高有效（经 R12→Q3） |

### SD 卡（SDIO 4-bit 总线）
| 引脚 | 信号 | 说明 |
|------|------|------|
| PC8 | SDIO_D0 | 数据 0 |
| PC9 | SDIO_D1 | 数据 1 |
| PC10 | SDIO_D2 | 数据 2 |
| PC11 | SDIO_D3 | 数据 3 |
| PC12 | SDIO_CK | 时钟（ClockDiv=30, ≈1.5MHz） |
| PD2 | SDIO_CMD | 命令 |
| PB3 | SDIO_DET | 输入，卡插入检测 |

> 实测：**不插 SD 卡上电不会卡死**（`MX_SDIO_SD_Init`/`fatfs_test` 对无卡容忍，不进 `Error_Handler` 死循环）。USB 模式写 SD 前若检测无卡，应经协议报错而非依赖复位路径。

### USB（OTG_FS，Device 模式）
| 引脚 | 信号 | 说明 |
|------|------|------|
| PA11 | USB_OTG_FS_DM | D- |
| PA12 | USB_OTG_FS_DP | D+ |
| PA8 | USB_CTRL | 输出，**USB 枚举使能**：拉高后 USB 才枚举（已实测确认；CubeMX 生成后在 main.c MX_GPIO_Init 里置高）。 |

### 按键（低电平按下，内部上拉；装配统一在 app_main.c）
| 引脚 | 信号 | 说明 |
|------|------|------|
| PA0 | SW_UP | 上翻 |
| PB13 | SW_DOWN | 下翻 |
| PB14 | SW_OK | 确认（短按选择/长按确认） |

### UART（115200-8N1）
| 引脚 | 信号 | 说明 |
|------|------|------|
| PA9 | USART1_TX | **日志输出**（gen_log → bsp_uart_log，CubeMX 已配）。V1.5.2 起日志走 USART1 |
| PA10 | USART1_RX | 预留 |
| PA2 | USART2_TX | 预留（V1.5.0~V1.5.1 曾作日志口，已切回；引脚现归杂项/未初始化） |
| PA3 | USART2_RX | 预留 |

### 目标 SWD（烧录目标芯片）⭐ 脱机烧录器核心（PCB010-V3.0）
| 引脚 | 标签 | 说明 |
|------|------|------|
| PB6 | OUT_SWCLK | 输出 → 目标 SWCLK（DAP_config.h） |
| PB7 | OUT_SWDIO | 双向 → 目标 SWDIO（DAP_config.h） |
| PB5 | OUT_RESET | 输出 → 目标 nRESET，低有效（DAP_config.h） |

> 这些是本机驱动目标芯片的 SWD 线（GPIO 位带操作，软件时序），与调试本机用的 PA13/PA14(SWD) 是两套。
> V2 板曾是 PB8/PB9/PB7（见 §5）。

### 调试 / 时钟源
| 引脚 | 信号 | 说明 |
|------|------|------|
| PA13 | SYS_SWDIO | 调试本机 |
| PA14 | SYS_SWCLK | 调试本机 |
| PH0 | RCC_OSC_IN | HSE 16MHz 晶振 |
| PH1 | RCC_OSC_OUT | HSE 16MHz 晶振 |

### 杂项 GPIO 输出（V3 预留，推挽输出、默认低）
| 引脚 | 网络（原理图） | 说明 |
|------|------|------|
| PC0 | OUT1 | 预留 |
| PC1 | OUT2 | 预留 |
| PB1 | OUT3 | 预留 |
| PB2 | OUT4 | 预留 |
| PB10 | OUT5 | 预留 |
| PB12 | BUZ_CTRL ⚠️ | 原理图蜂鸣器控制网络；但固件蜂鸣器走 PC6 PWM——**待核**：V3 原理图 PB12 经三极管驱动蜂鸣器？若为有源蜂鸣器则 PC6 方案需复核 |
| PB15 | OUT_VCC3V3_CTRL | 目标侧 3.3V 电源使能（目标供电控制，暂默认断电/低） |

---

## 3. 外设清单

| 外设 | 用途 | 关键参数 |
|------|------|---------|
| TIM3_CH1 | 蜂鸣器 PWM | PC6，PSC=83 → 1MHz tick，ARR 按 freq 重算（bsp_buzzer，不经 CubeMX） |
| SPI1 | LCD（主机） | Full-Duplex, /8=10.5MHz（ST7789 时序内） |
| SDIO | SD 卡 | 4-bit 总线, ClockDiv=30（≈1.5MHz）。原 ClockDiv=5 读 OK，但 **USB CDC + SD 写同跑时写 FIFO 欠载 → f_write 间歇 FR_DISK_ERR**；降速给 CPU 喂 FIFO 余量。硬件流控 ENABLE 触发 F401 SDIO errata（开则写立即失败）→ 保持 DISABLED。提速留 SDIO DMA。 |
| USART1 | 目标通信/预留 | PA9/PA10, 115200-8-N-1（CubeMX） |
| USART2 | **日志** | PA2/PA3, 115200-8-N-1（bsp_uart，不经 CubeMX） |
| USB_OTG_FS | USB 设备 | Device_Only |
| SYS | SWD 调试 + SysTick | — |

---

## 4. 特别注意事项

- **PC13/PC14/PC15 驱动能力有限**：最大约 2MHz、灌/拉电流 ~3mA。LED 必须用高阻值限流（低电流），GPIO 速度用 `GPIO_SPEED_FREQ_LOW`。代码见 `bsp_gpio.c`。
- **PC14 = LED_G 占用了 OSC32_IN**：本板**没有 32.768kHz LSE 晶振**，RTC（若用）需走 LSI(32kHz)。
- **蜂鸣器驱动（V3）**：固件走 PC6 TIM3_CH1 PWM。⚠️ V3 原理图另有 PB12=BUZ_CTRL 网络（Q4 低边开关）——若板载为有源蜂鸣器+三极管方案，应改 PB12 电平驱动而非 PC6 PWM；实测时以"上电嘀声"为准，不对则回退 PB12（见 §7 待确认）。
- **目标 SWD 电平**：PB6/PB7 为 3.3V 逻辑，烧 5V/1.8V 目标时注意电平匹配。

---

## 5. V2 → V3 引脚变更总表（2026-08-28，固件 V1.5.0）

| 功能 | V2（旧） | V3（现） | 固件位置 |
|------|---------|---------|---------|
| TFT_CS | PA3 | **PB0** | app/app_main.c 宏块 |
| TFT_DC | PB15 | **PC4** | 同上 |
| TFT_RST | PB14 | **PC5** | 同上 |
| TFT_BL | 无 | **PC7**（高有效） | 同上 |
| 蜂鸣器 | PA2 TIM2_CH3 | **PC6 TIM3_CH1** | bsp/bsp_buzzer.c |
| SW_OK（确认键） | PA15 | **PB14** | app/app_main.c（装配统一于此） |
| SW_UP（上翻） | PC6 | **PA0** | 同上 |
| SW_DOWN（下翻） | PC7 | **PB13** | 同上 |
| 目标 SWCLK | PB8 | **PB6** | dap/DAP_config.h |
| 目标 SWDIO | PB9 | **PB7** | 同上 |
| 目标 nRESET | PB7 | **PB5** | 同上 |
| 日志 UART | USART1 PA9/PA10 | **USART2 PA2/PA3**（115200） | bsp/bsp_uart.c |
| USART1 | 日志 | PA9/PA10 保留（目标通信预留，CubeMX 已配） | stm32f401_proj |
| 杂项输出 | — | **PC0/PC1/PB1/PB2/PB10/PB12/PB15**（推挽、默认低） | app/app_main.c |
| 未变 | LCD SPI（PA5/PA7）、SD（PC8-12/PD2/PB3）、USB（PA11/12/PA8）、LED（PC13/14）、调试 SWD（PA13/14） | 同左 | — |

> ⚠️ **CubeMX 未同步**：`.ioc` 仍是 V2 引脚（TIM2/PA2 蜂鸣器、PA15 按键、PB8/PB9 SWD、PC6/PC7 按键等）。
> 当前靠 app/bsp 层在 `app_main_init()` 运行时重配置纠正。下次开 CubeMX 改外设前，先把引脚改齐，
> 否则重新生成会回退 V2 配置（参考 README「防 CubeMX 重生成回退」的教训）。

---

## 6. ⏳ 待确认项

- [ ] 蜂鸣器：V3 板载蜂鸣器类型（有源/无源）与驱动网络（PC6 PWM vs PB12 BUZ_CTRL/Q4）。以实测为准。
- [ ] PB15（OUT_VCC3V3_CTRL）：目标侧 3.3V 电源使能——烧录时是否需要拉高给目标供电？当前默认低（断电）。
- [ ] PC6（OUT_VCC5V_CTRL，V3 原理图）：固件现用作蜂鸣器 PWM —— ⚠️ 若 V3 该脚确为目标 5V 电源使能，蜂鸣器需挪回 PB12，PC6 归电源控制。**与原理图复核，冲突待解**。
- [ ] 目标 SWD 连接器（FPC1）引脚顺序与烧录线序。

---

## 7. 维护说明

- **MCU 引脚映射以 `.ioc` 为准**：在 CubeMX 改引脚后重新生成，并同步更新本文件第 2 节。
- 外部硬件（驱动电路、连接器、型号）变更时更新第 4/5/6 节。
- 原理图版本：`PCB010-V3.0.pdf`（仓库根目录）。
