# 脱机烧录器 (Offline Programmer)

> **作者：嵌入式贫僧AI** · 微信号：**EmbMonkAI** · B站/抖音/视频号/公众号全网同名
> 开源地址：https://gitee.com/EmbMonkAI/offline_prog_open
>
> 基于 **STM32F401RB** 的脱机烧录器工程。脱离 PC 端工具，由 MCU 自主完成目标芯片的烧录/校验流程。
> **本工程全程使用 AI 闭环开发**——从需求拆分、逐模块编程、PC 模拟测试到编译烧录、串口断言验收。

**📚 联系作者**：微信号 **EmbMonkAI**（备注「烧录器」拉你进交流群）；B站/抖音/视频号/公众号搜「**嵌入式贫僧AI**」。有问题随时找我，看到都会回；也欢迎 Issue 讨论。

<!-- TODO: 成品演示 GIF 占位（拍摄后替换） -->

**它能做什么**：插 SD 卡选固件 → 按键选择 → 自动烧录+回读校验目标芯片（SWD 10MHz，支持 3700+ 款 ARM Cortex-M），屏幕显示进度与结果，全程无需 PC。

**配套课程**：《AI 闭环开发嵌入式》——以本项目为载体，讲透"如何用 AI 把一个 MCU 项目从需求推到实物"的可迁移方法论。
讲师：**嵌入式贫僧AI**，开源仓库持续更新，欢迎关注催更。

---

## 0. 关于本仓库

- **读保护(RDP)处理为预留接口**（见 `app/app_rdp.c`）——开源版不含自动解锁实现，
  如你手上的目标芯片开了读保护，欢迎到视频评论区讨论你的芯片型号
- 加密体系（AES-CMAC/设备 UID 绑定）密钥在设备不在代码，开源不影响安全性；
  默认口令 `DONECHIP` 仅用于教学演示，量产请自行修改
- License: MIT（见 [LICENSE](LICENSE)），传播请保留「嵌入式贫僧AI」来源信息，方便获取后续更新

---

## 1. 硬件信息

| 项目 | 说明 |
|------|------|
| 主控 MCU | STM32F401RBTx |
| 系统时钟 | 84 MHz (HCLK) |
| 调试接口 | ST-Link (SWD) |
| 提示音 | 无源蜂鸣器，PA2 = TIM2_CH3，**2.7 kHz** PWM |
| SD 卡 | SDIO 4-bit（PC8-12 / PD2），实测 8GB SDHC |
| 串口日志 | USART1（PA9 / PA10），115200-8-N-1 |
| 显示屏 | 1.54" TFT ST7789 240×240（SPI1），背光引脚见 [PCB010-V3.0.pdf](PCB010-V3.0.pdf) |

### 蜂鸣器 PWM 参数（TIM2_CH3 / PA2）

| 参数 | 值 | 说明 |
|------|-----|------|
| Prescaler (PSC) | `83` | 84MHz/(83+1)=1MHz，1 tick = 1µs |
| Counter Period (ARR) | `369` | 周期 370µs → ≈2703 Hz |
| Pulse (CCR) | `185` | 50% 占空比 |
| 启停代码 | `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3)` | |

---

## 2. 目录结构

```
offline_prog/
├── README.md            本说明
├── .gitignore
├── app/                 应用层代码（用户业务逻辑）
├── bsp/                 板级支持包（外设驱动封装）
├── stm32f401_proj/      CubeMX + MDK-ARM 主工程
│   ├── stm32f401_proj.ioc   CubeMX 工程文件
│   ├── Core/                main.c / it / msp / system
│   ├── Drivers/             HAL + CMSIS
│   └── MDK-ARM/             Keil 工程 (.uvprojx)
└── modules/             嵌入式框架（随本仓库分发）
    ├── app/ bsp/ component/ device/ service/ sim/ cmake/
    └── CMakeLists.txt
```

---

## 3. 工具链

| 工具 | 用途 |
|------|------|
| **STM32CubeMX** | 图形化配置引脚/时钟/外设，生成初始化代码 |
| **Keil MDK-ARM (µVision)** | 编译、链接、下载、调试 |

---

## 4. 快速开始

### 4.1 硬件准备

| 物品 | 说明 |
|------|------|
| 本项目 PCB（V3.0） | 原理图/布局见 [PCB010-V3.0.pdf](PCB010-V3.0.pdf)，主控 STM32F401RBT6 |
| ST-Link V2 | 烧录本机固件 + 调试 |
| SD 卡 | FAT32 格式，放 `.opfp` 镜像文件 |
| 目标板 | 任意 SWD 可烧录的 ARM Cortex-M（开源版不含 RDP 自动解锁，已开读保护的芯片需先手动解锁） |
| USB 数据线 | PC → 烧录器，传输 `.opfp`（CDC 虚拟串口）；也可直接拷 SD 卡 |

### 4.2 PC 端工具

**方式 A（推荐，免装 Python）**：到仓库 [Releases](https://gitee.com/EmbMonkAI/offline_prog_open/releases) 下载打包好的 `opfp_generator.exe`，双击即用。

**方式 B（从源码运行）**：

```bash
git clone https://gitee.com/EmbMonkAI/offline_prog_open.git
cd offline_prog_open/tools/opfp_gen
pip install PySide6 pyserial
python opfp_generator.py
```

工具用法：选厂家/型号 → 导入固件 .bin →（可选）配滚码/次数 → 导出 `.opfp` → **拷到 SD 卡** 或 USB 线连设备点「发送到设备」。

### 4.3 编译固件

1. 用 Keil 打开 `stm32f401_proj/MDK-ARM/stm32f401_proj.uvprojx`
2. `F7` 编译 → `F8` 下载（ST-Link）
3. 修改外设配置：打开 `stm32f401_proj.ioc` → 调整 → 重新生成代码（注意保留 `USER CODE BEGIN/END` 区块）

> 字库已内置固件（`app/ui_font_*.h`），无需在 SD 卡放任何字库文件。

---

## 5. 引脚分配

| 引脚 | 功能 | 外设 | 说明 |
|------|------|------|------|
| PA2 | PWM 输出 | TIM2_CH3 | 蜂鸣器驱动（2.7 kHz） |
| PC13 | GPIO 输出 | — | 红灯 LED_R，**低电平点亮** |
| PC14 | GPIO 输出 | — | 绿灯 LED_G，**低电平点亮** |
| PC8-PC12, PD2 | SD 总线 | SDIO | SD 卡 D0-D3 / CK / CMD（4-bit） |
| PA9 / PA10 | UART | USART1 | 串口日志（115200-8-N-1） |
| PA15 | GPIO 输入 | — | 按键 BTN **确认键**（低电平按下）|
| PC6 / PC7 | GPIO 输入 | — | 按键 **UP / DOWN**（选烧录文件，低电平按下）|
| PA5 / PA7 | SPI 主 | SPI1 | LCD SCK / MOSI（10.5MHz）|
| PA3 / PB15 / PB14 | GPIO 输出 | — | LCD CS / DC / RST（ST7789 240×240 TFT）|
| PA13/PA14 | SWD | — | 调试下载 |
| PA11 / PA12 | USB | USB_OTG_FS | D- / D+（CDC 虚拟串口，PC↔MCU 文件传输）|
| PA8 | GPIO 输出 | — | USB_CTRL，**拉高为 USB 枚举使能** |
| PB7 | GPIO | — | 目标 nRESET（保持高阻，不驱动；SWD 复位走 AIRCR）|

> 后续分配的引脚请在此表持续登记，便于团队协作。

---

## 6. 开发日志

> 按时间倒序记录，每条注明日期、内容、状态。

### 2026-08-29 — 镜像编号体系 + .opfp v5 全链路（V1.8.0~V1.8.4，两端五轮实测）✅

**目标**：SD 卡多文件时按编号快速定位；v5 编程配置段（PGCF）设备端全链路；PC 端连接状态显示。用户真机五轮迭代全部实测通过（CH32F103）。

**.opfp v5 / PGCF 段（28B，紧跟 16B 显示名后）**
- 字段：magic "PGCF" + flags（滚码/次数上限/加密占位/镜像编号）+ 滚码四字段 + 次数上限 + image_id（详见 `tools/opfp_gen/DEVELOPMENT.md §15`）
- **MCU V1.8.0**：`opfp.h` 加 `opfp_pgcf_t`；parse/烧录路径识 v5（按 version 跳段 + magic 校验，坏段拒烧码 11）
- **段长判别坑**：初版 v5 段 24B（无 image_id）——判段长必须用 header 自带 `fw_size+algo_size` 反推总长（`pgcf_len_of`），不能用 len 阈值（algo+fw 长时旧文件同样超阈值，判错 blob 错位）

**镜像编号（产线定位）**
- PC：编号 1~9999 默认 1（永有编号）；**发送文件名强制 `NNN-` 前缀**（编号唯一化，且 SD 目录天然按编号排序）
- MCU：列表行首**黄底黑字「NNN」编号块**（曾青底白字对比不足；曾 23px 裁成「00」→26px）；列表**按编号升序**（FAT 目录序覆盖/删除后乱）；烧录页名称行**无前缀**（与 PC 输入显示名完全一致）
- **编号唯一**：MCU 收 `NNN-xxx.opfp` 前删 SD 上同号不同名旧文件（`dedup_image_id`）——重发同号 = 更新该号镜像，设备每号唯一
- 烧录页参数 5→7 行：新增**滚码**（`地址@起始值`）/ **限制**（次数上限，未启用 `-`），布局重排（名称 y25 / 参数 y43..138 / 进度条 y141..168）

**只认 v5（V1.8.2 起）**：v4/v3 旧文件列表不显示 + parse/run2 拒绝（码 4）——PC 工具已统一产 v5，旧版一律视为不合法

**PC GUI 增量**：设备连接状态行（2s 轮询 CDC 口，绿「已连接 COMx」/灰「未连接」，插拔沿打日志）；**「发送到设备」仅连接时可用**；空显示名 v5 文件不再整文件消失（设备回退显示文件名）

**踩坑记录**：①空显示名 + 旧列表判据「空名=v3 剔除」→ v5 空名文件整文件消失（V1.8.1 修：v4/v5 空名回退文件名）②两次发送同输出路径 → SD 同名 `FA_CREATE_ALWAYS` 无提示覆盖（V1.8.2 修：编号前缀）③ASCII 字体 8px 宽，3 位数字 24px > 23px 块宽被裁（V1.8.2 修：26px）

### 2026-08-30 — 安全体系 V1.9.0~V2.1.2（滚码/次数执行→加密架构三轮演进→真机闭环）✅

**本日跨度**：从 v5 配置「能显示」到全套安全体系真机闭环，固件 V1.9.0→V2.1.2 共 13 个版本。完整安全档案（漏洞/方案对比/选型理由/攻防验证）见 `tools/opfp_gen/DEVELOPMENT.md §16`。

**功能落地**
- 滚码执行（V1.9.0）：验证通过后经 algo program_page 写 serial_addr + 回读校验 + 游标持久化（键=serial_addr，跨授权不重号）；擦除范围扩到滚码页
- 次数上限执行（V1.9.0）：烧前比对设备 flash 计数，超限拒烧（码 13），烧录页红字 N/M! 预警
- 计数权威源定稿（V1.12）：设备 flash（nonce 键）——SD 零写入、回滚免疫、换卡不丢
- **设备端加密（V2.1 定稿架构）**：PC 生成明文 v6（零密码学）→ 厂家 USB 导入 →
  设备收完用 K=f(口令,UID) 整文件 AES-CTR 加密 + CMAC 落盘（ENC1 格式）。
  SD 拷出无法提取固件；改任何字节拒烧；跨设备解不开；口令持久化 Sector5。
  PC GUI 新增「口令」输入框（随 BEGIN 帧下发，空=默认口令）

**架构演进关键决策**
- V1.13 PC 端加密 → V2.0 设备端加密（用户主导）：PC 零密码学消灭「反编译 PC 得密钥」攻击面
- 密钥来源用户定稿：口令+UID 双因子（口令隔离产品线，UID 锁设备）
- CMAC 单一共享实现（流式 begin/update/end）：两份"等价"内联副本实测算出不同结果后重构

**真机攻防验证**：SD 拷出提取固件❌ / 改密文❌ / 明文直拷❌ / 换口令旧文件❌ / 跨设备❌——全部正确拒绝

**排障实录**（详见 DEVELOPMENT.md §16.4）：栈溢出复位、LFN 堆耗尽（512B→4KB）、
CMAC 副本分歧、重构误删写头、f_tell 异常值、printf 半主机挂死、flash 裸写静默丢弃——
每条都是真机实证后修复。调试残留已全部清理（Code -772B，0 警告）。

**其他**：删除 gen_key.py/opfp_sec.py（旧 PC 加密路线遗物，V2.1 无消费者）；
USB 传输 EOF 超时 3s→20s（设备加密需 ~8s）；硬故障取证打印（SCB+栈回溯）与
复位源打印保留为产线诊断。

### 2026-08-29 — PC 端 GUI v5 产线化改造（四区布局 + 滚码/次数/加密）✅

**目标**：`opfp_generator.py` 按产线使用流重构——厂家→型号两级下拉选型；导入固件实时显示 FLASH 占用/校验和；界面分四区（芯片信息/编程配置/文件导入导出/操作日志）；`.opfp` 升 v5（24B 编程配置段）。31 项自动化断言全过。

**四区布局（2×2）**
- **① 芯片信息**：厂家下拉（ST 1193 / 沁恒WCH 3）→ 型号下拉（可输入过滤 + 补全，1193 款不卡）；选中即显 flash 起始/页大小/总容量；导入固件后 `FLASH 占用: 33.7KB/64KB (54%)`（超容红字）+ `FLASH 校验和: 0x????????`（CRC32）实时刷新
- **② 编程配置**：滚码（递增序列号：flash 地址 + 宽度 1/2/4B + 起始值 + 步进，越界/未对齐生成时拒绝）、编程次数（最大烧录上限，超限 MCU 拒烧——对接 V1.7.0 计数持久化）、编程加密（占位：开关+密钥框，算法未定不加密落盘，勾选日志黄字提示）
- **③ 文件导入导出**：导入原始固件 / 导出 .opfp v5 / **导入 .opfp 回填**（反查型号按 flash 三参数 + algo blob 字节级匹配；F401RB/F401CB 等完全同参型号弹窗让用户确认）+ USB 发送到设备
- **④ 操作日志**：每次操作带时间戳留痕，成功绿/失败红/警示黄，限 2000 行——用户放心可靠

**.opfp v5 = v4 + 24B PGCF 配置段**（magic "PGCF" + flags + 滚码 4 字段 + 次数上限；段结构见 `tools/opfp_gen/DEVELOPMENT.md §15`）。MCU 固件暂只认 v4——v5 适配（解析跳段 + 次数比对拒烧 + 滚码写入）为下个固件里程碑。

**测试**：`tests/test_gui_v5.py`（offscreen 冒烟 + v5 字节布局断言 + 导入回填往返 + 约束拒绝 + v4 兼容），31/31 PASS。

### 2026-08-29 — V1.7.x 烧录计数持久化 + 三层 flash 写入排障实录 ✅

**目标**：每个 `.opfp` 文件累计烧录次数掉电不丢；烧录页新增「次数」行，成功后 +1 刷新。附带把「烧录中 USB 传输」从超时挂等改成显式忙拒。全部真机实测通过（CH32F103 连烧计数 0→1→2，断电重启保持）。

**功能设计（V1.7.0）**
- **存储**：STM32 内部 flash **Sector4 顶部 4KB**（`0x0801F000`，固件仅占 Sector0-3 的一部分，隔 Sector 边界互不干扰）——256 槽 × 16B 记录 `{magic, name_hash, count, crc32}`，append-only 追加；**压缩重写**：写满后每 hash 只留最新值擦除重写，擦写次数 = 烧录次数/256，支撑 ~250 万次烧录
- **查询/递增**：`app_count_get/inc`（`app/app_count.c`），文件名 FNV-1a 散列；烧录成功（含回读校验通过）才 +1；掉电安全：半写只丢最后一条（CRC 校验剔除坏记录，最多少计 1 次）
- **UI**（`app_ui.c`）：烧录页参数区 4 行 → 5 行（新增「次数」，行距 16→15px），成功后局部重画次数行
- **USB 忙拒**（V1.7.0 附带）：烧录期间 `app_usb_set_busy(1)` → BEGIN/DATA/EOF 回 `STAT_BUSY(0xF5)`（PC 端 `usbproto.py`/`opfp_transfer.py` 同步识别重试），不再 3s 超时 ×N 干等；`on_progress` 捎带 `app_usb_poll()` 消化环防溢出

**排障实录（V1.7.1，三层问题叠加——值得记录的排查路径）**

症状：CH32F103 烧录成功，次数始终 0。

1. **残留错误标志堵门（第一层）**：串口 `[E][count] count: program FAIL st=1 halerr=0x8 sr_pre=0x20`——`sr_pre=0x20`（PGAERR）在调 HAL 之前就置位。根因：**Keil/JLink 下载固件的 flashloader 残留错误标志**，`HAL_FLASH_Program` 入口（`FLASH_WaitForLastOperation`）见脏标志直接拒写。修复：编程前在**解锁窗口内**清全部错误标志（`flash_err_flags_clear()`，EOP/OPERR/WRPERR/PGAERR/PGPERR/PGSERR）——注意 **LOCK 状态下写 SR 清标志静默无效**，必须 `HAL_FLASH_Unlock()` 之后清。
2. **x64 并行度违规（第二层）**：清完标志 V1.7.1 仍失败，`halerr=0xA`（PGAERR|PGSERR）、入口干净。根因：RM0090 并行度-电压表——**x64 双字编程要求 VDD≥2.7V**，而本板 JLink 实测 **VTref≈2.45V**（欠压）。修复：改 **x32 字编程 ×4**（2.4V+ 合法）+ 擦除电压档同步降 `FLASH_VOLTAGE_RANGE_2`。⚠️ 本板 3.3V 供电偏低（LDO/USB 压降）待查——84MHz+2WS 在 <2.7V 下的读操作也超规格，偶发 FR_DISK_ERR 可能同源
3. **DCache 不失效（第三层，预防性修复）**：F4 的 `HAL_FLASH_Program` 路径**不刷 DCache**（`FLASH_FlushCaches` 仅 Erase 路径调用），`app_count_get` 读回缓存的旧空槽 0xFF → 同一供电周期内永远显示 0。修复：编程后 `FLASH_FlushCaches()` + 写后回读 memcmp 自证。

**方法论沉淀**：flash 写入失败先打**寄存器现场**（SR/CR + HAL 错误码，进入前后各一次）再定位——本次三层问题（残留标志/并行度/缓存）靠 `sr_pre/sr_post/halerr` 一次实测一层层剥开，避免盲改。版本号 V1.7.1（启动横幅 `boot V1.7.1` 可验真固件）。

### 2026-08-16 — .opfp v4 显示名 + 烧录页打磨 + 固件版本号（V1.0.0→V1.4.4）✅

**目标**：PC 端为每个固件填一个「烧录器显示短名」（≤16 英文字符，与文件名解耦）；烧录页布局/显示细节四轮打磨；固件带上版本号。全部真机实测通过（含 CH32F103 烧录）。

**.opfp v4 显示名**（V1.1.x）
- PC GUI（`opfp_generator.py`）：新增「显示名称」输入框（≤16 可打印 ASCII，非法字符即时过滤）；`.opfp` v4 = v3 112B header + **16B 名称段**（\0 填充，不参与 CRC）+ algo + fw
- MCU（`target/opfp.h` / `app_flash.c`）：`app_flash_parse` 读名称（v3 无名段自动跳过 seek）；`app_flash_run2` 按版本跳过名称段读 algo
- UI（`app_ui.c`）：列表/烧录页优先显示名；**V1.4.0 起只认 v4**——v3 旧文件不进列表（PC 工具统一 v4，不做旧兼容）
- 踩坑：显示名与文件路径共用一个指针数组，确认键拼出 `0:显示名` 打不开文件（V1.1.1 修：路径一律用真文件名数组）

**烧录页四轮显示打磨**（V1.2.x~V1.4.x，全部直写屏刷新策略问题，与 RAM 无关）
- 布局改版（V1.3.0）：第二行显示烧录名称（黄字）；状态并入进度条（待机灰字/烧录中/成功**整条绿**/失败**整条红**）
- 修「保护」乱码：值是 GBK 中文「开/关」却走 ASCII 字库——值字体按首字节自动选
- **防闪烁三部曲**：①进度条增量填充（只画新增段）②百分比数字只刷小块+去重 ③条上文字改**透明 blit**（`area_pixel` 透明背景，不铺底色——`rect_text` 写字前会用 bg 铺满矩形，是"中空条/双进度"反复出现的总根因）
- 修快结束时不一致（V1.4.4）：填充右缘穿过数字区（85~95% 段）时，数字小块**按右缘劈两半清底**（青|黑），与主条逐像素对齐
- 文件选择页：标题栏右侧「当前/总数」序号（如 1/10），换选只刷小块

**固件版本号**（V1.2.0 起）
- `app_main.h` 集中定义 `FW_VERSION_STR` + 版本史注释；上电第一行打印 `=== offline_prog boot V1.4.4 ===`
- 每次功能修改递增（major.不兼容/minor.功能/patch.修复）——排查"烧的是不是新固件"一眼可辨

**方法论沉淀**：直写屏（无帧缓冲）的显示优化 = 把每次重画压到最小区域 + 避免任何"铺底色"操作覆盖已变化内容；`rect_text` 的铺底语义在动态区域是坑，动态元素一律透明 blit。

### 2026-08-16 — TFT 屏真机点亮 + 三键 UI 脱机烧录全流程 ✅

**目标**：接入 1.54" 240×240 TFT（ST7789），文件选择→确认→烧录进度/结果全屏上操作，脱离串口看日志。**端到端实测通过（CH32F103 烧录 OK）**。TFT 驱动层的完整演进（含花屏/黑屏排查实录）记录在开发日志中。

**硬件接入（pcb010-V2.0 飞线验证）**
- LCD：SCK=PA5/SDA=PA7（硬件 SPI1，10.5MHz——ST7789 规格 16MHz 内留裕量）、CS=PA3、DC=PB15、RST=PB14
- 新增按键：**UP=PC6、DOWN=PC7**（选文件，内部上拉低有效）；PA15 改**确认键**
- 排查实录：底部花屏（逐字节翻 CS 打断命令参数 → 事务化单 CS）；红区黑线（超长单事务 → run 8 行上限 + DISPON 后预热）；"时好时坏"最终定案为**飞线接触/串扰**（整理接线后完全正常）——教训：同代码不同现象=随机误码，先查物理连接

**应用层 UI（app/app_ui.c，两页流程）**
- P1 文件选择：标题「文件选择」（16×16 汉字）+ SD 卡 .opfp 逐行列表；UP/DOWN 换选（40ms 去抖 + 首按即响应 + 按住 0.5s 后 300ms 连发），选中行反白 + 红「→」
- P2 程序烧录：标题 + 关键参数（芯片/固件大小/起始地址/读保护）+ 进度条 + 状态行（按确认开始→烧录中 xx%→烧录成功绿底/失败红底）+ 底部文件名；确认执行烧录，结束再按确认回列表
- **防闪屏**：换选只局部重画两行（~3ms，不整屏 clear）；进度回调只重画进度条+状态行——§13.16 教训（持续全屏重写会打异常这块屏）的落地
- 中文串显式 GBK 字节；字库 `kp_font_lib.bin` 放 SD 根目录（无则文字缺失，色块照常）

**支撑改动**
- `app_flash.c`：`app_flash_run2(path, progress_cb)`（指定文件+进度回调，每 2KB 报百分比）；`app_flash_parse()` 导出 header 参数供 P2 显示；旧 `app_flash_run()` 保留兼容
- `app_main.c`：TFT 硬件装配（tft_setup()，原 test_tft_hw.c 移入）自检后接 app_ui_init()，主循环 app_ui_poll()（替换旧"按键直接烧"）
- 分层修正：**文件列表/烧录页等业务画面全部在 app/ 层**，modules 保持通用（曾误放 modules 测试代码里，已迁回）
- Keil：app_ui.c 入编；.ioc 同步（PC6/PC7 输入上拉、SPI1 分频）——防 CubeMX 重生成回退

**遗留**：burn_count 持久化（当前显示 1）；烧录中 USB 暂停响应（同步烧录）；字库无 16×16 ASCII（文件名为 8×16 小骨架，需要时补生成）

### 2026-08-11 — USB CDC 文件传输打通（Phase 1-4）✅

**目标**：PC GUI 生成 `.opfp` 后直接经 USB 发给 MCU 写入 SD，免手动拷卡。Phase 1-4 完成，端到端实测通过（PC→USB→SD→短按→SWD 烧录 STM32F407 SUCCESS）。原计划 HID+CDC 复合，评估后**简化为 CDC 单通道**（控制/状态并入协议帧，避免复合描述符手术）。

- **USB 设备侧（CubeMX CDC）**：接入 ST USB Device Library CDC 类；**PA8(USB_CTRL) 拉高为枚举使能**（实测必要）；PC 识别为 COM 口（VID 0x0483 / PID 0x5740）
- **CDC 双向管道**：`usbd_cdc_if.c` `CDC_Receive_FS`（loopback 验证 PASS，`tests/test_cdc_loopback.py`）
- **文件传输协议（停等式 BEGIN/DATA/EOF + 端到端 CRC32）**：`app/app_usb.c/.h`——IRQ 收→环形缓冲(2KB)→主循环解析→`f_write` 写 SD→ACK；写失败 ACK 带 FatFs 错误码（`0xDEAD0000|FR`）；协议规范见 `tools/opfp_gen/DEVELOPMENT.md §14`
- **PC GUI「发送到设备」**：`opfp_generator.py` 抽 `_build_opfp_bytes()`（落盘/发送共用）+ 按钮 + 进度条；`usbproto.py`（PC 协议单一份）+ `opfp_transfer.py`（QThread worker，失败整体重试 `MAX_RETRIES=3`，`FA_CREATE_ALWAYS` 幂等）
- **SDIO 写可靠性修复**（全工程首次 SD 写）：USB+SD 写同跑偶发 `FR_DISK_ERR`（写 FIFO 欠载）→ SDIO 降速 `ClockDiv=30`(≈1.5MHz) 缓解 + PC 重试兜底；硬件流控触发 F401 SDIO errata 不用；**根治留 SDIO DMA**（见 DEVELOPMENT.md §10.6、HARDWARE.md「SDIO」）
- **硬件确认**：PA8=USB 枚举使能；无 SD 卡上电不卡死

**Phase 5（未做，parked）**：长按进 USB 模式 + 收完自动烧（按 BEGIN 文件名，新增 `app_flash_run_path()`，不走 `find_opfp`，消除多 `.opfp` 歧义）+ LED/蜂鸣器反馈；可选 MCU 回传 STATUS 帧让 PC 显示烧录进度/结果。


### 2026-08-10 — probe-rs 烧录实测通过 + F1 Keil 算法 + RDP 方案B ✅
- **CH32F103 实测通过**：probe-rs 芯片库全链路（PC assemble → .opfp v3 → MCU 烧录）成功，有/无读保护均自动处理
- **BKPT header 修复**：probe-rs 算法 blob 须保留 BKPT header（`0xBE00BE00`）作返回陷阱 —— `swd_flash_syscall_exec` 靠 `LR=breakpoint` 命中 BKPT 指令 halt（无 FPMBP 软件断点），剥 header 会卡在 algo init
- **F1 系列切 Keil F1 算法**：probe-rs F1 在沁恒 CH32F103 上慢（ProgramPage ~1-2s/次，flash 时序不匹配）；F1（CH32F1/STM32F1 Med ≤128KB）改用 Keil F1 algo（自带 BKPT，秒级），其余 probe-rs
- **RDP 方案B（参数 MCU 内置）**：RDP 参数固化 MCU（`app_rdp.c` 的 `RDP_TABLE`，按 rdp_type：1=F1式/2=F4式）；`.opfp` 只传 rdp_type（build 按族自动标）；GUI 删「启用读保护」按钮 —— MCU 自动检测目标加锁，有保护才 mass-erase 解锁，用户无需判断
- **芯片库范围**：启用 ST（STM32）+ 沁恒（CH32F1 ARM，1196 款）；其他厂家 yaml 在 `probe_rs_targets/off` 隐藏，按需移出再 build
- **输出名带型号**：GUI 生成 `.opfp` 自动命名「固件名-芯片型号.opfp」
- **PC 端开发文档**：`tools/opfp_gen/DEVELOPMENT.md`（架构 / assemble 映射 / .opfp v3 / F1 特例 / RDP 机制概览）

### 2026-08-10 — probe-rs 芯片库集成 + .opfp v3 + 删除手填 algo ✅
- **PC 端芯片库**：集成 probe-rs 244 个芯片系列 YAML（`tools/opfp_gen/probe_rs_targets/`），覆盖 **3682 款 ARM Cortex-M**（ST/兆易GD/沁恒/雅特力/合泰/SiliconLabs/NXP/Microchip/Renesas/英飞凌/TI...）
- **build_chip_db.py**：YAML → Python 复刻 probe-rs `assemble_from_raw_with_data`（BKPT header / load_address-4 / stack-buffer 布局）→ 保留 BKPT header 作返回陷阱、适配 `program_target_t` → 去重输出 `chips.json`（3706 芯片共享 272 算法，~3MB）
- **GUI v3**：`opfp_generator.py` 树形选型（厂家→系列→型号 + 搜索），选中自动填 flash 参数 / 算法入口 / 总大小，生成 `.opfp`
- **.opfp v3**：header 加 `flash_size`（108→112B，版本 2→3）；MCU 端 `app_flash.c` 早失败 + `target_program.c` 权威校验 `fw_size > flash_size`（防擦除越过 flash 末尾，新错误码 `ERROR_SIZE`）
- **MCU 执行逻辑不动**：probe-rs 的 BKPT-in-blob 与现有 breakpoint-字段是等价机制，保留已验证的 `swd_flash_syscall_exec`（省心 + 低风险）
- **删除旧做法**：手填 algo（`dap/algo/STM32F10x_128.c`、`STM32F4xx_1024.c`）、`target_db.c`、`dap/SWD_flash.[ch]`、`gen_opfp.py`；`.uvprojx` 同步移除引用
- **PC 端开发文档**：新增 `tools/opfp_gen/DEVELOPMENT.md`（架构 / assemble 映射 / .opfp v3 / 更新芯片库 / 验证）
- RDP：暂不处理（用户手动解决），GUI 仅留占位开关
- 验证：chips.json 结构校验 + GUI v3 header 打包测试通过；**MCU 回归待 F1 实烧**（probe-rs CH32F1 算法烧 CH32F103）

### 2026-08-10 — 方案 B：RDP 参数数据化 + STM32F4xx 支持 + .opfp v2 ✅
- **.opfp 格式升级到 v2**（header 72B → 108B）：新增芯片保护参数段（类型 + 参数，格式见 .opfp 文档）
- **保护处理重构为数据化**：按芯片族分发（F1 式 / F4 式）；参数全部从 .opfp 文件取（开源版未含解锁实现）
- **target_chip_t 加 rdp_params_t**：RDP 参数成为芯片对象属性（方案 B）
- **PC 工具加 STM32F4xx 预设**：algo blob(90 words) + F4 Flash 参数(16KB 扇区) + F4 RDP 参数(OPTKEY=0x08192A3B/0x4C5D6E7F, OPTCR 模式)
- **MCU 固件彻底不认识具体芯片**：RDP 参数从文件来，加新芯片族（如 F0/G0）只需 .opfp 带 type=1 参数
- 实测 F1(CH32F103)：v2 .opfp → RDP 自动解锁 → 擦除+编程 33748B → DONE + 滴1声

### 2026-08-09 — Phase 4：PC 端 OPFP 生成工具（PySide6 GUI）✅
- 新增 `tools/opfp_gen/opfp_generator.py`：PySide6 GUI 工具
  - 芯片型号下拉（预设 CH32F103/STM32F103，含 algo 数据，后续可加芯片库）
  - Flash 参数自动填充（flash_start / page_size / has_rdp），可手动修改
  - 固件文件选择（浏览 .bin → 显示大小）
  - 一键生成 `.opfp`（header + algo blob + firmware + CRC32）→ 拷到 SD 卡即可烧录
- 同时保留命令行版 `gen_opfp.py`（脚本/自动化用）
- 使用流程：打开 GUI → 选芯片 → 导入 bin → 生成 .opfp → 拷 SD 卡 → 按键烧录
- 实测：GUI 可正常打开、生成 .opfp，MCU 端读取烧录正常

### 2026-08-09 — Phase 3：工程文件 .opfp（MCU 固件通用化）✅
- 定义 **`.opfp` 二进制格式**（`target/opfp.h`）：72B header（magic + 版本 + 芯片参数 + algo 描述 + CRC32）+ algo blob + firmware
- **PC 端工具** `gen_opfp.py`：从 algo + 固件合成 `.opfp`，输出到桌面
- **重写 `app_flash.c`**：不再找 `.bin`、不再用编译期硬编码参数；改为找 `.opfp` → 读 header → **RAM 构建** `target_chip_t` + `program_target_t` → 流式烧录
- **MCU 固件彻底通用化**：不认识任何具体芯片，换芯片只需换 `.opfp` 文件（零改固件）
- 实测：读 `.opfp` → 自动解锁 RDP → 擦除 + 编程 33748B → DONE + 滴1声

### 2026-08-09 — Phase 2：algo 数据化（algo 从全局单例 → 芯片对象属性）✅
- `target_chip_t` 加 `const program_target_t *algo` 字段；`chip_stm32f1.algo = &flash_algo`
- **重写 `target_program.c`**：不再调 dap/SWD_flash.c 的 `target_flash_*`，直接用 SWD_host 原语（`swd_write_memory` + `swd_flash_syscall_exec`）+ `chip->algo` 控制全流程（halt→下载algo→init→erase→program）
- **移除 `dap/SWD_flash.c`**（不再编译，逻辑已搬到 target 层）—— dap 模块只提供 SWD 原语，**不认识任何具体芯片**
- algo 从"dap 全局单例"变为"芯片对象里的指针"，为 Phase 3（工程文件携带 algo blob）铺路

### 2026-08-09 — Phase 1：芯片对象抽象（target 层）✅
- **目标**：把散落在 app_flash/app_rdp 里的芯片参数（flash_start/page_size/has_rdp）收敛成只读 `target_chip_t` 对象，烧录逻辑与芯片/传输解耦
- 新增 `target/` 目录（与 app/bsp/dap 平级）：
  - `target.h`：`target_chip_t`（芯片对象）+ `transport_ops_t`（传输接口，SWD/UART 可插拔）+ `target_session_t`（运行时会话，一拖N基础）
  - `target_db.c`：`chip_stm32f1` 对象（flash_start=0x08000000, page_size=1KB, has_rdp=1）
  - `transport_swd.c`：薄包装 dap/SWD_host 成 `swd_ops`（connect/disconnect/read/write）
  - `target_program.c`：通用烧录流程 `target_program_begin/page/end`（connect→检查→init algo→erase / program page / reset RUN）
- **重写 `app/app_flash.c`**：只管"找 .bin + 逐页读 SD"，烧录全走 target 层；芯片参数来自对象不再硬编码
- 功能不变（实测 F1 烧录正常），但结构对了——**后续加芯片只需 target_db.c 加一个对象**

### 2026-08-09 — 读保护(RDP)自动解锁 + SWD 提速 10MHz ✅
- **读保护处理**（app 层，调 dap SWD 接口）：
  - added app_rdp.c: protected-target detection & handling (full unlock impl not in open-source build)
  - `app_flash` 烧录前自动检测：未保护→跳过；保护目标按策略处理（代价：整片擦除目标）；永久保护→失败
  - dap 暴露 SWD 原语接口：`swd_read_word`/`swd_write_word` 去 static、补 `swd_write_halfword`，在 `SWD_host.h` 声明（**仅接口，RDP 算法在 app**）
- **SWD 时钟 4MHz → 10MHz**（`DAP_config.h` `DAP_DEFAULT_SWJ_CLOCK`），烧录提速
- 实测：保护/非保护目标均按策略正常处理

### 2026-08-09 — 脱机烧录核心（SWD/DAP）+ nRESET 大坑 ✅
- 引入 DAP 模块（CMSIS-DAP 核心 + SWD host + flash algo），目标 **STM32F1xx**（F1 算法，bin 从 0x08000000 起）
- 新增 `app/app_flash.c`：按键触发 → 根目录找 .bin → `swd_init_debug` → `target_flash_init` → 按 1KB 页擦除 → 按页 `program_page` → 复位运行；返回 0=成功/非0=失败码；成功滴1声、失败连续滴3声
- SWD 引脚 PB8=SWCLK、PB9=SWDIO；编译优化改 **-O0**（SWD 软件位时序标定值，对齐参考工程）
- **⚠️ nRESET 大坑（浪费大量调试时间，重点记录）**：
  - **现象**：SWD 能连上（读 IDCODE=0x2BA01477 成功），但 **AP 内存访问失败**（`swd_read_memory` 返回 0）→ `target_flash_init` 报 `ERROR_RESET(3)`
  - **弯路**（全都不是根因）：比对 dap 代码（一致）、优化级别（-O0）、关中断、UART vs RTT 打印 —— 逐个排除，耗时很长
  - **真正原因**：**PB7（目标 nRESET）被程序员推挽驱动，与目标复位电路冲突** → AP 访问失败。**断开 RST 线即正常**
  - **解决**：**不驱动 nRESET**（PB7 设为输入高阻 / 物理不接 RST）；SWD 复位走**软件 AIRCR**（`swd_set_target_reset`），不依赖硬件 nRESET。参考工程也是不接 nRESET
  - **教训**：STM32 上"**DP 通（IDCODE 能读）、AP 不通（内存访问失败）**"，要**首先怀疑目标复位/电源状态**（nRESET 被拉低/驱动冲突），而不是 SWD 代码/时序

### 2026-08-09 — 按键 dev_button（IO 直连）✅
- 评估：矩阵 `dev_keyboard`(扫描) 与 IO 直连按键(直读) 机制不同 → **新建 `dev_button`**（与 dev_keyboard 平级），不改矩阵模块
- 新增 `modules/device/button/dev_button.[ch]`：**一个按键一个对象**（对齐 dev_led，无 count），`port/pin` 为值（非指针），`active_low` 极性，raw 读电平（防抖/事件留 service 层）；`dev_button_read` 返回 1=按下 / 0=释放
- `bsp/bsp_gpio` 加 `bsp_gpio_set_input_pullup`（上拉输入，按键常用）
- app：PA15 按键实例（接 GND + 内部上拉，`active_low=1`），主循环 20ms 防抖读取，边沿打印 `BTN: pressed/released`
- PA15 默认 JTDI，SWD 调试下已释放，运行时配为上拉输入（无需 CubeMX 重生成）
- 实测：按 PA15 串口正确输出按下/释放

### 2026-08-09 — FATFS 文件系统 + 长文件名（LFN）✅
- CubeMX 生成 FATFS（SD 模式）：`Middlewares/Third_Party/FatFs`（ff.c 等）+ `FATFS/Target`（`bsp_driver_sd.c` 把 diskio 接到 HAL_SD）
- app `fatfs_test()`：`f_mount` + 列根目录 + 读 `test.txt`；实测读出 `DongleV207_comb.bin`(33748B)、`test.txt`(39B)
- **长文件名**：CubeMX 开 `_USE_LFN=2`（stack 动态缓冲）→ `FILINFO.fname` 直接返回长名（这版无需 lfname 指针），`DongleV207_comb.bin` 完整显示（不再是 `DONGLE~1.BIN`）
- **SD 卡信息修复**：`HAL_SD_GetCardInfo` 从 init 开头挪到 `f_mount` 之后——卡由 `BSP_SD_Init` 在 `f_mount` 时才完整初始化，此前读到全 0；现显示 `type=1(SDHC) blk=512 count=15126528 (7386 MB)`
- 测试函数统一返回 `0=成功 / 非0=失败码`（`fatfs_test` 透传 FatFs `FRESULT`，如 `FR_NOT_READY=3`）
- SD 裸块读写自检 `#if 0` 保留；LED 仅作心跳；测试结果只看串口

### 2026-08-09 — SDIO 调试 + 串口日志（gen_log）✅
- 串口日志：套用 `modules/component/gen_log` 统一日志抽象，启用 UART 端口
  - 新增 `bsp/bsp_uart.[ch]`：`bsp_uart_log()` = vsnprintf + `HAL_UART_Transmit`(USART1)
  - `.uvprojx` 加 `GEN_LOG_PORT_UART` 定义 + gen_log include；app 用 `gen_log_info/err`
  - 日志经 USART1（PA9/PA10, 115200-8-N-1）输出
- SD 卡（SDIO 4-bit，PC8-12/PD2；CubeMX `MX_SDIO_SD_Init` 已配）：
  - `HAL_SD_GetCardInfo` → type=1(SDHC) class=1461 blk=512 count=15126528 = **8GB 卡**(7386 MiB)
  - 读写自检 `sd_rw_test()`：保存原扇区→写 pattern→读回→memcmp 比对→恢复（不破坏卡内容；缓冲 `uint32_t[128]` 4 字节对齐）
  - 反馈：串口日志为主；PASS=LED 交替/蜂鸣器静音，FAIL=双灯常亮+蜂鸣器长鸣
  - 实测：**`SD R/W: PASS`** ✅ —— SDIO（init + 数据读写）全通
- ⚠️ `MX_SDIO_SD_Init` 无卡会进 `Error_Handler` 死循环（到不了 app），脱机烧录器后续需做"无卡容错"
- 待办：集成 FATFS（核心放 `modules/fs/fatfs`，diskio 放 `bsp/`）以读 `firmware.bin`

### 2026-08-09 — 蜂鸣器驱动（dev_buzzer 通用抽象 + bsp_buzzer PWM 适配）✅
- 蜂鸣器：无源，PA2 = TIM2_CH3（CubeMX 已配 PSC=83/ARR=369/Pulse=185 → 2.7kHz/50%）
- 新增 modules 设备抽象 `device/buzzer/dev_buzzer.[ch]`：`drive` 字段支持
  - PWM 驱动（无源）：freq=音调、duty=音量，注入 hw_on/hw_off
  - 电平驱动（有源/驱动电路）：active_low 极性，注入 gpio_set_high/low（极性规则同 dev_led）
  - 统一 API：init/on/off/set_freq/set_duty/beep/get_state（电平模式自动忽略 freq/duty）
- 新增 `bsp/bsp_buzzer.[ch]`：TIM2_CH3 PWM 适配，hw_on 按 dev->freq 算 ARR、dev->duty 算 CCR 后 `HAL_TIM_PWM_Start`，hw_off 停止
- app：`s_buzzer`(drive=PWM, freq=2700, duty=50) 注入 bsp_buzzer；上电"嘀"一声提示音
- 编译 0 error，烧录试听正常（变调/音量可调）

### 2026-08-09 — 通用 bsp_gpio + 删除 bsp_led ✅
- 评估后确认：GPIO 应「实现一次、注入所有设备」（dev_led / dev_lcd / …），不应每设备重写
- 新增 `bsp/bsp_gpio.[ch]`：通用 STM32 HAL GPIO（`set_high/set_low/set_output/set_input/get_level` + 时钟使能），`uint32_t pin` 对齐设备结构
- `app_main.c` 的 LED 改为注入通用 `bsp_gpio_*`
- 删除 `bsp_led.h/.c`（极性已在 dev_led，GPIO 已通用，无遗留职责）
- `modules/bsp/bsp_gpio.h` 的 pin 为 uint16_t（与设备结构 uint32_t 不一致），本工程按 uint32_t 实现，**不改 modules**
- 编译验证：0 error（dev_led / bsp_gpio / app_main 均干净）；2 warning 仍为既有 #1296
- Code size 10070 → 12830（新增链接 `HAL_GPIO_ReadPin` 等读取相关代码，128KB Flash 下可忽略）

### 2026-08-09 — dev_led 增加极性字段 active_low ✅
- 经讨论确认：极性属于「LED 设备实例」的属性，应放在 `dev_led_dev_t`，而非在 BSP 反相
- 改 `modules/device/led/dev_led.h`：新增 `active_low` 字段 + `DEV_LED_ACTIVE_HIGH/LOW` 宏
- 改 `dev_led.c`：新增 `dev_led_apply()` 按极性选电平，`init/on/off/toggle` 统一走它
- `bsp_led.c` 改回**字面电平**（`set_high` 真高 / `set_low` 真低），不再反相；BSP 回归通用
- `app/app_main.c`：两个 LED 实例填 `.active_low = DEV_LED_ACTIVE_LOW`
- 编译验证：0 error；2 warning 均为既有 `#1296`（静态初始化里 port 指针转 uint32_t），与极性无关
- ⚠️ 注意：本次改动位于 modules/ 框架目录，随本仓库一同提交

### 2026-08-09 — LED 驱动接入（dev_led + bsp 适配）✅
- 原理图确认：红灯 PC13 / 绿灯 PC14，均**低电平点亮**
- 新增 `bsp/bsp_led.[ch]`：为 dev_led 提供平台 GPIO 函数（STM32 HAL），并在 BSP 层屏蔽 active-low 极性（框架假设高电平点亮，本板相反）
- 在 `app/app_main.c` 通过 dev_led 接口驱动红绿 LED：上电红亮/绿灭，主循环每 500ms 红绿交替闪烁
- `main.c` 的 USER CODE 区域接入 `app_main_init()` / `app_main_loop()`
- 说明：极性初版在 bsp 反相处理；后改为 dev_led 的 `active_low` 字段（见上条），modules/ 随之更新

### 2026-08-09 — 工程初始化 ✅
- 用 STM32CubeMX 生成 **STM32F401RB** 的 MDK-ARM 工程（`stm32f401_proj`）
- 系统时钟配置为 **84 MHz**
- 配置 **PA2 = TIM2_CH3**，2.7 kHz PWM 用于蜂鸣器提示音（PSC=83, ARR=369, Pulse=185）
- 引入嵌入式框架代码（modules/ 目录）
- 建立 `app/`、`bsp/` 分层目录（暂空，待填充）
- 初始化 git，添加 `.gitignore`（屏蔽 MDK-ARM 编译中间产物），撰写本 README

### _后续模板_
```
### YYYY-MM-DD — 标题
- 改动点 1
- 改动点 2
- 状态 / 待办
```

---

## 7. Git 约定

- 作者统一显示为 **嵌入式贫僧AI**（`EmbMonkAI@noreply.gitee.com`）
- 换行符统一见仓库根 `.gitattributes`（`* text=auto`）
- 提交信息格式：`类型(范围): 摘要`，如 `feat(usb): ...` / `fix(ui): ...` / `docs: ...`

---

## 8. 相关链接与致谢

- **作者：嵌入式贫僧AI** · 微信号：**EmbMonkAI** —— B站 / 抖音 / 视频号 / 公众号全网同名，欢迎关注交流
- 开源地址：https://gitee.com/EmbMonkAI/offline_prog_open （Issues / PR 欢迎）
- 芯片算法库源自 [probe-rs](https://github.com/probe-rs/probe-rs) 目标定义（Apache-2.0/MIT）
- DAP 固件核心参考 [ARM CMSIS-DAP](https://github.com/ARM-software/CMSIS_5)（Apache-2.0）
- STM32CubeMX：https://www.st.com/en/development-tools/stm32cubemx.html
