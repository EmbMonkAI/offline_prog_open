# OPFP PC 端工具 — 开发文档

> 脱机烧录器的 PC 端：从 probe-rs 芯片库选型 → 自动 assemble → 生成 `.opfp` 工程文件 → 拷 SD 卡，或经 **USB CDC 直传 MCU 写 SD**（见 §14）。
> 本文档记录 PC 端的设计与开发细节，供后续维护。

> **当前进度（2026-08-11）**：USB CDC 文件传输 Phase 1-4 完成，端到端实测通过
> （PC 生成 .opfp → USB 发送 → MCU 写 SD → 短按 SWD 烧录 STM32F407 SUCCESS）。
> Phase 5（长按进 USB 模式 + 收完自动烧 + 烧录结果回传 PC）parked。详见 README §6。

---

## 1. 概述

PC 端工具用 **probe-rs 的芯片 YAML** 作为唯一数据源，覆盖 **3700+ ARM Cortex-M 型号**。
不再手填任何算法/芯片参数（旧 `CHIP_DB` 与 `dap/algo/*.c` 已删除）。

核心思路：
- **数据 + assemble 完全用 probe-rs**：YAML 数据源 + 复刻 probe-rs 的 `assemble_from_raw_with_data`
- **assemble 后剥掉 BKPT header**，适配 MCU 端已有的 `program_target_t` 字段
- **MCU 端执行逻辑不动**（`swd_flash_syscall_exec` 已验证可用）—— 这是「省心」的关键

> 为什么 MCU 执行逻辑不动？probe-rs 在算法 blob 前塞 BKPT header（`0xBE00BE00`），
> 本工具用独立的 `breakpoint` 字段（`algo_start|1`）—— 两者是同一机制的等价写法。
> 重写已验证的 MCU 执行=费心且引入风险；数据源 assemble 化才是「省心」的真正来源。

---

## 2. 目录结构

```
tools/opfp_gen/
├── opfp_generator.py      # PySide6 GUI（树形选型 + 生成 .opfp v3 + 「发送到设备」USB 传输）
├── opfp_transfer.py       # USB 传输 QThread worker（停等协议 + 失败整体重试）
├── usbproto.py            # USB CDC 文件传输协议（PC 端单一份，对应 MCU app_usb.c；见 §14）
├── build_chip_db.py       # 从 YAML 提取 → chips.json（assemble 复刻，开发/更新芯片库时跑）
├── chips.json             # 生成的芯片库（GUI 数据源；chips + algos 去重）
├── requirements.txt       # PySide6 + PyYAML + pyserial
├── tests/                 # CDC loopback / 文件传输测试脚本（pytest 友好）
├── probe_rs_targets/      # probe-rs 的 244 个芯片系列 YAML（数据源，可重建 chips.json）
└── DEVELOPMENT.md         # 本文件
```

---

## 3. 数据流

```
probe_rs_targets/*.yaml   ──┐
   (probe-rs ChipFamily)     │  build_chip_db.py
                             ├─► chips.json {chips, algos}
                             │     (ARM 过滤 + assemble + 去重)
                             │
   GUI(opfp_generator.py) ◄──┘
     ├─ 树形选型（厂家→系列→型号）
     ├─ 选中 → chips[name] + algos[algo_id] → 填参数
     └─ _build_opfp_bytes() → .opfp v3 (112B header + algo blob + firmware)
                                     │
                ┌──────────────────────┴─────────────────────────┐
       方式A 拷 SD 卡                                  方式B USB CDC 直传
       （手动拷贝）                       opfp_transfer.py + usbproto（见 §14）
                │                                          │ BEGIN/DATA/EOF + CRC32
                ▼                                          ▼
                          MCU 读/写 .opfp（FATFS "0:"）
                                  → 构建 target_chip_t → SWD 烧录
```

---

## 4. chips.json 结构

去重设计：同 assemble 结果的算法只存一份，芯片只引用 `algo_id`。
3682 芯片共享 271 个算法（去重 93%），文件约 1 MB。

```json
{
  "chips": {
    "STM32F401RB": {
      "vendor": "ST", "family": "STM32F4", "algo_name": "stm32f4xx_128",
      "flash_start": 134217728, "flash_size": 131072, "page_size": 16384,
      "ram_start": 536870912, "ram_size": 65536, "algo_id": "a1b2c3d4e5f6"
    },
    ...
  },
  "algos": {
    "a1b2c3d4e5f6": {
      "algo_start": 536870916, "algo_code": [1859, 33816577, ...],
      "algo_init": 536870945, "algo_uninit": ..., "algo_erase_chip": ...,
      "algo_erase_sector": ..., "algo_program_page": ...,
      "algo_breakpoint": 536870917, "algo_static_base": ...,
      "algo_stack_pointer": ..., "algo_program_buffer": ..., "algo_program_buf_sz": 1024
    },
    ...
  }
}
```

---

## 5. assemble 映射（核心技术）

复刻 `probe-rs/probe-rs/src/flashing/flash_algorithm.rs` 的
`assemble_from_raw_with_data`（约 L260-461），关键步骤：

| 步骤 | probe-rs 做法 | 本工具适配 |
|------|--------------|-----------|
| 算法代码 | `[BKPT header] + FLM原始代码` | **保留 header**（`0xBE00BE00`）作返回陷阱 |
| addr_load | `load_address - 4`（给 header 让位） | 同 probe-rs（PIC 时 = `ram_start`） |
| algo_start | — | `= addr_load`（blob **含 header** 下载到此） |
| 入口 pc_* | `code_start + offset`（code_start=addr_load+4） | `= code_start + offset`（同 probe-rs） |
| static_base | `code_start + data_section_offset` | 同 probe-rs |
| stack / page_buffer | probe-rs 布局（code 之后放 data/buffer，RAM 顶端栈） | 直接采用 |
| breakpoint | 藏在 header（`0xBE00BE00`） | `= addr_load \| 1`（指向 BKPT header） |

**为什么必须保留 BKPT header？** 本工程 `swd_flash_syscall_exec` 的 halt 机制是：
设 `LR=breakpoint`，CPU 执行完算法 `BX LR` 跳到 breakpoint，**该地址必须有 BKPT 指令
（`0xBE00`）才会 halt** —— 它没有 FPMBP 软件断点，只靠 `swd_wait_until_halted` 轮询。
所以 blob 开头必须有 BKPT header，`breakpoint` 指向它（`addr_load`）。

> ⚠️ 曾尝试「剥 header、breakpoint=algo_start|1」—— 实测卡死在 `algo init`：
> breakpoint 落在普通代码（如 `0xB510`=PUSH）上，CPU 返回后执行代码永不 halt。
> 这正是 probe-rs 塞 `0xBE00BE00`、旧手填 `0xE00ABE00`（小端 `[00 BE..]`→`0xBE00`=BKPT）
> 的共同原因 —— **blob 开头必须有 BKPT 作返回陷阱**。

### 字段语义辨析（易混淆）

probe-rs 的 `flash_properties` 有两个「大小」：
- `page_size` —— **编程页**（`ProgramPage` 一次写入的字节数，如 F4 = 0x400）
- `sectors[0].size` —— **擦除扇区**（如 F4 = 0x4000）

本工具映射到 MCU（`target_chip_t.page_size` 同时用于擦除步进与读文件粒度）：
- `chips.page_size` = **`sectors[0].size`**（擦除扇区步进）
- `algos.algo_program_buf_sz` = **`flash_properties.page_size`**（编程页）

> 注意：沁恒 CH32F1 在 probe-rs 里 page_size/sector 都是 0x80（WCH 烧录器的 128B 粒度），
> 与 STM32F1 的 1KB 页不同 —— 这是 probe-rs 的定义，烧录可成功（擦除按 0x80 步进，幂等）。

---

## 6. .opfp v3 格式（112 B header）

在 v2（108B）基础上，`fw_size` 后新增 `flash_size`，版本 2→3。MCU 端 `sizeof` 整读，
加字段自动跟上。`#pragma pack(push,1)`，字段顺序即 ABI。

| 偏移 | 类型 | 字段 | 说明 |
|------|------|------|------|
| 0 | u32 | magic | 0x50464F4C |
| 4 | u16 | version | **3** |
| 6 | u16 | flags | bit0=RDP |
| 8 | u32 | flash_start | 0x08000000 |
| 12 | u32 | page_size | 擦除扇区大小 |
| 16 | u32 | fw_size | 固件字节数 |
| **20** | **u32** | **flash_size** | **flash 总大小（v3 新增）** |
| 24 | u32×12 | algo_* | start/size/init/uninit/erase_chip/erase_sector/program_page/breakpoint/static_base/stack_pointer/program_buffer/program_buf_sz |
| 72 | u16 | rdp_type | 0=none,1=F1式,2=F4式 |
| 74 | u16 | reserved | |
| 76 | u32×8 | rdp_* | fpec_base/key1/key2/optkey1/optkey2/ob_addr/ob_val/sr_bsy_mask |
| 108 | u32 | crc32 | CRC(偏移 0~107) |

> RDP 段：rdp_type 由 build 按芯片族自动标（F1→1），.opfp 自动带；rdp_* 参数填 0
> （MCU 内置，不用）。GUI **无「启用读保护」按钮** —— MCU 自动检测，有保护才解锁（mass-erase）。

---

## 7. 更新 / 扩展芯片库

### 从 probe-rs 同步新 YAML
1. 从 `probe-rs/probe-rs/targets/` 拷新 YAML 到 `probe_rs_targets/`
2. `python build_chip_db.py` 重新生成 `chips.json`
3. 重启 GUI 即可看到新芯片

### 新增厂家识别
编辑 `build_chip_db.py` 的 `VENDOR_MAP`（芯片名前缀 → 厂家）。
匹配是 `startswith` 顺序匹配，**具体/长前缀要放前面**。

### assemble 失败的芯片
`build_chip_db.py` 对 RAM 太小装不下「算法+栈+缓冲」的芯片会跳过
（如 TLE987x、LPC55S69、STM32L4A6 等，日志打印 WARN）。
这些芯片不进 `chips.json`。如需支持，需手动调整 assemble 的 stack/布局。

---

## 8. 运行

```bash
pip install -r requirements.txt       # PySide6 + PyYAML
python build_chip_db.py               # 仅首次/更新芯片库时跑
python opfp_generator.py              # 打开 GUI
```

GUI 流程：左侧树选 厂家→系列→型号（或搜索）→ 固件浏览 → 输出路径 → 生成。
生成的 `.opfp` 拷到 SD 卡根目录，按键即烧。

---

## 9. 验证 / 自检

### PC 端
- `build_chip_db.py` 内置结构校验：入口地址必须在 RAM 内、字段非空等
- GUI 生成时做 `fw_size > flash_size` 拦截（与 MCU 端一致）

### MCU 端回归（终极验证）
**F1 实烧**：F1 系列用 Keil F1 算法（见第 11 节），选 CH32F103C8T6 / STM32F103C8 生成 `.opfp` 烧录。
成功（滴 1 声）= assemble + BKPT 执行机制全链路正确。
**F4 实烧**（2026-08）：选 STM32F407VG 生成 `.opfp` 烧录通过（13244B，6×2048+956）。
首次验证 **probe-rs 原生算法**（非 Keil 覆盖）在目标上执行——证明 assemble + BKPT 机制对
F4（Cortex-M4）同样成立。过程中踩到两个坑（见第 13 节），均已修。
**RDP 回归**：勾选「启用读保护」烧读保护的 CH32F103，验证自动检测 → mass-erase 解锁 → 烧录。

> F1 用 Keil 算法（非 probe-rs），不能用「与 probe-rs yaml 逐字节比对」自检。实烧才是真验证。
> F4 是首个纯 probe-rs 算法实烧通过的系列——「assemble + BKPT 执行」通用性从单系列扩到跨系列。

---

## 10. 已知限制

1. **仅 ARM Cortex-M**：RISC-V（CH32V 等）/ Xtensa（ESP32）被过滤（SWD 烧不了）
2. **芯片库范围**：当前只启用 ST（STM32）+ 沁恒（CH32F1 ARM，共 1196 款）；其他厂家 yaml 在 `probe_rs_targets/off` 隐藏，需要时移出再 build
3. **F1 系列用 Keil 算法**：probe-rs F1 在沁恒芯片上慢，F1（CH32F1/STM32F1 Med ≤128KB）走 Keil F1 覆盖（见第 11 节）
4. **RDP 参数 MCU 内置**：`app_rdp.c` 的 RDP_TABLE 按 rdp_type 存（F1式/F4式），.opfp 只传 rdp_type（族标识），GUI **无「启用读保护」按钮**（MCU 自动检测 + 按需解锁，见第 12 节）
5. **小 RAM 芯片跳过**：少数芯片 assemble 失败（见第 7 节），不进库
6. **SDIO 写偶发 FR_DISK_ERR**（Phase 3 USB 写暴露，全工程首次 SD 写）：USB CDC + SD 写同跑时，
   `f_write`/`f_open` 偶发返回 `FR_DISK_ERR`（写 FIFO 欠载，详见 HARDWARE.md「SDIO」）。
   当前缓解：① SDIO 降速 `ClockDiv=30`（≈1.5MHz）；② PC 端传输整体重试（`opfp_transfer.py` `MAX_RETRIES=3`）。
   硬件流控 ENABLE 触发 F401 SDIO errata（开则写必失败）→ 不用。**根治：SDIO DMA**（DMA 喂 FIFO 不抢 CPU，顺便提速）——留作后续硬化。

---

## 11. F1 系列特例：Keil F1 算法 + RDP 内置参数

probe-rs 的 F1 算法在沁恒 CH32F103 上烧录极慢（每次 ProgramPage ~1-2s，flash 时序不匹配）。
本工具对 **F1 系列（CH32F1 / STM32F1，Med-density ≤128KB）** 做覆盖，其余 3700 款仍 probe-rs：

- **算法** `KEIL_F1_ALGO`（从旧 CHIP_DB 提取的 Keil F1 blob，已验证烧 CH32F103 快）
  - 自带 BKPT：`algo_code[0]=0xE00ABE00`，小端 halfword 0xBE00=BKPT，`breakpoint=algo_start|1`（无 header 风格）
  - `page_size=0x400`（1KB 物理页，Med-density）
- **RDP**：F1 系列标 `rdp_type=1`（族标识，参数 MCU 内置，见第 12 节）

实现见 `build_chip_db.py` 的 `parse_chip_family`：F1 系列 `entry.update(KEIL_F1_ALGO)` + `entry["rdp_type"]=1`。
`.opfp` 自动带 rdp_type；rdp_* 参数填 0（MCU 内置不用）；GUI 无按钮，用户无需选。

> 混合方案：F1 用 Keil，其他 probe-rs。若日后 probe-rs F1 修好或找到更快 algo，可去覆盖统一回 probe-rs。

---

## 12. 读保护（RDP）机制概览（多厂家扩展参考）

「读保护」概念几乎所有 MCU 都有，但**实现机制、存储位置、解锁方式差异大**。ST 的 RDP（选项字节式）只是其一。

**本工具方案（RDP 参数 MCU 内置）**：RDP 参数固化在 MCU（`app_rdp.c` 的 `RDP_TABLE`，按 rdp_type），
`.opfp` 只传 rdp_type（族标识，build 按芯片族自动标，用户无感）。**GUI 无「启用读保护」按钮** ——
MCU 端 `target_program_begin` 自动检测目标是否加锁，有保护才解锁（mass-erase），无保护跳过。
理由：用户在 PC 端无从判断产线上那颗芯片是否加锁，让"默认都检测"才对；检测本身只读寄存器，无害。
当前 RDP_TABLE 内置 type 1（F1 式）+ type 2（F4 式），覆盖 ST 全系 + 国产对标。加新族改 MCU（RDP_TABLE 加一行）。

### 四大类机制

| 类型 | 典型厂家 | 存储 | 解锁 | 可逆 |
|---|---|---|---|---|
| **选项字节式**（ST 式）| ST、沁恒/兆易/雅特力 | 选项字节区 | 写 RDP 值 → mass-erase | Level1 可逆，Level2 永久 |
| **flash 安全位** | NXP Kinetis(FSEC)、Silicon Labs(DLW)、Nordic(UICR.APPROTECT)、Microchip SAM(GPNVM) | flash 配置字/锁字 | mass-erase + 清位 | 多数可逆 |
| **eFuse 一次性** | Espressif ESP32、部分 NXP | 硬件熔丝 | 不可清 | **永久** |
| **密码/ID 鉴权** | Renesas RX(ID code)、TI MSP430(JTAG 密码) | flash/OTP | 调试口输密码 | 可逆 |

### 典型差异（非 ST 厂家怎么解锁）

- **Nordic nRF52/nRF53**：`UICR.APPROTECT` 写 0 锁 SWD；解锁要「mass-erase + 单独擦 UICR」两步。
- **NXP Kinetis**：FTFA `FSEC[SEC]` 位；解锁发 FTFA unlock + mass-erase 命令（非写选项字节）。
- **Silicon Labs EFM32**：flash 里的 `DLW(Debug Lock Word)`，擦它前得先解锁 flash（套娃）。
- **Espressif ESP32**：eFuse 烧死，**不可解锁**（不能 mass-erase 清）。

### 扩展非 ST 厂家

每家协议不同（连 SWD 方式、命令序列、是否要密码都不同），不是加几个参数能解决，而是每家一个解锁模块（如 `app_rdp_nordic.c`）。
当前定位 ST 系 + 国产对标，覆盖面已很大；Nordic/NXP/ESP32 等按出货场景需要再加。

### RDP 参数参考来源（无集中数据库）

- 各芯片 **RM 的 Flash 章节**（最权威）
- **ST CMSIS Device headers**（GitHub `STMicroelectronics/STM32Cubexx`）的 FLASH 寄存器 `#define`
- **OpenOCD** `src/flash/nor/stm32*.c` 的 RDP 解锁代码（参数最全）
- **pyOCD** STM32 target 代码

---

## 13. 踩坑记录（F4 实烧，2026-08）

F407 首次实烧连续踩到两个 bug。两者共同点：**数据源（probe-rs YAML）是对的，
错的是本工程消费数据的逻辑**。记此节，避免扩展新芯片系列时重蹈。

### 13.1 算法加载到不可执行区（CCM）—— `algo init FAIL`

**现象**：F407 在 `algo init` 阶段 FAIL（超时），F1 正常。

**根因**：STM32F407 在 probe-rs YAML 的内存映射里，**首个 RAM 区是 CCMRAM
(0x10000000)，且标记 `access.execute: false`**；可执行的主 SRAM 在后面(0x20000000)。
`stm32f4xx_1024` 算法无 `load_address`（PIC），加载地址 = `ram_start`。
`build_chip_db.py` 的 `rams[0]` 盲目取首个 RAM 区 = CCMRAM，**没复刻 probe-rs
跳过 `execute:false` 的逻辑**，把算法下到了 CCM。

STM32F4 的 CCM 只挂 CPU 的 D-bus，I-code 取指总线访问不到——SWD 能把算法写进 CCM
（数据写成功，所以日志不报 download FAIL），但 CPU 取第一条指令即 bus fault →
HardFault 死循环 → `swd_wait_until_halted()` 永不满足 → "algo init FAIL"。
反汇编证明 init 函数无任何忙等待、必返回 0，FAIL 只能是 core 根本没跑起来。

**修复**：`build_chip_db.py` RAM 区选择跳过 `access.execute == false`（与 probe-rs 一致）。
F407 改加载到 0x20000000 主 SRAM。MCU 固件无需改（地址完全由 .opfp 数据驱动）。

> STM32F3 的 0x10000000 是可执行 SRAM（YAML 未标 execute:false），不受影响、非回归。

### 13.2 读固件溢出 s_buf —— `programming...` 后无打印卡死

**现象**：init + erase 都通过，日志打印 `programming...` 后**无任何 `tgt: prog` 行**，
卡死 ≥2 分钟，无成功/失败打印。F1 不复现。

**根因**：`app_flash.c` 读 SD 的粒度误用 `s_chip.page_size`：
`page_size` 在本工程被复用为「擦除扇区步进」，F4 = 16KB。读循环却把它当读粒度，
往 `s_buf[FLASH_BUF_SIZE=2048]` 里 `f_read` 了 13244 字节 → 溢出 ~11KB，
冲烂紧随其后的 `s_algo_blob / s_algo / s_chip` 静态变量 → 后续 SWD 用乱码地址卡死。
F1 的 page_size=1KB(Keil) < 2048 不溢出，所以没暴露。

`page_size` 三个语义被混淆：擦除步进 / SD 读粒度 / 编程页(program_buffer_size)。
读粒度只应取决于缓冲区大小。

**修复**：读粒度改用 `FLASH_BUF_SIZE`，与 `page_size` 解耦。`target_program_page`
内部已按 `program_buffer_size` 分块编程，读块无需对齐 flash 页。MCU 固件需重编译。

### 13.3 经验

- **`page_size` 是扇区步进，不是缓冲区粒度**——任何「按 page_size 读写固定缓冲」都要核对缓冲容量。
- **RAM 区选择必须看 `execute` 标志**——probe-rs 标 `execute:false` 的区（F4 CCM）不能放可执行算法。
- **algo 字节数小是正常的**——probe-rs 算法刻意精简（无 SystemInit/时钟配置），F4 仅 336B；
  Keil FLM 因带时钟初始化才 ~2KB。algo 内容是否完整看反汇编入口，不看体积。

---

## 14. USB 传输协议（CDC，PC ↔ MCU）

PC GUI 生成 `.opfp` 后，经 USB CDC 把文件发给 MCU、MCU 写入 SD 的应用层协议。
**权威定义在代码**：MCU 端 `app/app_usb.h`（规范注释）+ `app/app_usb.c`（宏与实现，
`T_BEGIN`/`STAT_*` 等，约 L19-41）；PC 端参照实现 `tools/opfp_gen/tests/test_cdc_file.py`。
本节是描述性文档，若与代码不一致以代码为准。

### 14.1 为什么是这个设计

- **CDC 是字节流，无包边界** → 必须自加成帧。用 `0x55 0xAA` 帧头同步 + `len` 长度前缀，
  解析器失步时能重新对齐。
- **USB bulk 自带 CRC+重传**，链路层已可靠 → **不做 per-frame CRC**（省开销、解析简单），
  改用**端到端 CRC32**（整文件）在 EOF 校验，捕捉传输/写卡任何环节的损坏。
- **停等协议（stop-and-wait）**：PC 发一帧、等一个 ACK 再发下一帧。简单可靠，环形缓冲不会
  溢出（任一时刻在途最多一帧）。代价是吞吐受 RTT 限制（~256-512 KB/s），对本场景（~14KB-
  数十KB .opfp）足够。日后要提速可改滑动窗口。
- **控制/状态与文件数据共用一根 CDC 管道**，靠帧类型区分（不用 HID）。`T_CMD`(0x10) 预留给
  PC→MCU 命令（中止等，Phase 5）。

### 14.2 帧格式（字节级）

```
偏移  字段              说明
0     0x55             帧头同步字节 0
1     0xAA             帧头同步字节 1
2     type             帧类型（见 14.3）
3     len (低字节)      载荷长度（小端 u16）
4     len (高字节)
5..   payload[len]     载荷
```
所有多字节字段**小端**。一帧最小 5 字节（len=0）。

**解码示例**——`BEGIN "usbtest.bin" size=4000 crc=0x9590CC83`：
```
55 AA 01 14 00  0B 75 73 62 74 65 73 74 2E 62 69 6E  A0 0F 00 00  83 CC 90 95
└──────┘ └┘ └──┘  └────────── name(11) ──────────┘  └── size ──┘ └── crc32 ──┘
同步    type len=20  name_len=0x0B "usbtest.bin"       4000          0x9590CC83
```

### 14.3 帧类型

| 方向 | type | 宏 | 载荷 |
|------|------|----|----|
| PC→MCU | 0x01 | `T_BEGIN` | `name_len:u8` + `name[name_len]` + `size:u32` + `crc32:u32` |
| PC→MCU | 0x02 | `T_DATA` | `seq:u32` + `bytes[≤512]` |
| PC→MCU | 0x03 | `T_EOF` | 空 |
| PC→MCU | 0x10 | `T_CMD` | `cmd:u8`（保留，Phase 5 中止等）|
| MCU→PC | 0x81 | `T_ACK` | `ack_type:u8` + `status:u32`（EOF 额外带 `crc32:u32`，共 9 字节）|

### 14.4 ACK 语义（`ack_type` + `status`）

`ack_type`：`AT_BEGIN=0x01` / `AT_DATA=0x02` / `AT_EOF=0x03`（回显被应答的帧类型）。

`status` 含义随 `ack_type` 变：
- **BEGIN**：`STAT_OK(0x00)` = 已 f_open；`STAT_BAD(0xF0)` = 帧格式错；`STAT_OPENFAIL(0xF1)` = f_open 失败（多半无 SD 卡 / 未挂载）。
- **DATA**：正常 = **回显 seq**（PC 据此确认顺序）；写失败 = `0xDEAD0000 | FatFs错误码`（高位 `0xDEAD` 作标记，低 16 位是 FR 码，便于 PC 直接打印定位）。
- **EOF**：`STAT_OK(0x00)` = CRC/长度全匹配；`STAT_CRC(0xF4)` = 不符；并附带 MCU 实算的 `crc32`（9 字节载荷）。

FatFs 错误码（FR_*，`ff.h`）：`0 OK / 1 DISK_ERR / 2 INT_ERR / 6 INVALID_OBJECT /
10 WRITE_PROTECTED / 13 NO_FILESYSTEM / 15 TIMEOUT / 19 INVALID_PARAMETER`。
Phase 3 实测遇到的 `0xDEAD0001` = `FR_DISK_ERR`，根因见 HARDWARE.md「SDIO」行（写 FIFO 欠载，已降时钟修复）。

### 14.5 传输流程（停等）

```
PC                                MCU(app_usb_poll, 主循环)
│                                  │
│  BEGIN(name,size,crc32) ────────►│ f_open("0:name") → 累加器清零
│  ◄──────────── ACK(BEGIN,OK) ────┤
│  DATA(seq=0, 512B) ─────────────►│ f_write 512B, crc 累加
│  ◄──────────── ACK(DATA,seq=0) ──┤
│  DATA(seq=1, 512B) ─────────────►│ f_write ...
│  ◄──────────── ACK(DATA,seq=1) ──┤
│   ...（4000B = 7×512 + 416）...  │
│  DATA(seq=7, 416B) ─────────────►│ f_write 416B
│  ◄──────────── ACK(DATA,seq=7) ──┤
│  EOF ───────────────────────────►│ f_close，比对 累加crc == BEGIN给的crc
│  ◄──────── ACK(EOF,OK,mcu_crc) ──┤ status=0 匹配 / 0xF4 不符
```
DATA 块大小 `DATA_CHUNK_MAX=512`（对齐 SD 扇区，`f_write` 高效）。最后一个块可短。

### 14.6 CRC32（端到端校验）

- 算法：**CRC-32/ISO-HDLC**（反射，poly `0xEDB88320`，初值 `~0`，结束再 `~0`）——与 PC `zlib.crc32` **逐位一致**。
- MCU 实现：`app_usb.c` `crc32_update()`（无查表，逐字节 8 轮，~13KB 数据微秒级）。
- PC 端：`zlib.crc32(data) & 0xFFFFFFFF`。
- **校验点**：PC 在 BEGIN 带上整文件 crc；MCU 边写边累加，EOF 时比对，结果回传。
- **实测向量**：`bytes((i*7+3)&0xFF for i in range(4000))` → CRC32 = `0x9590CC83`（test_cdc_file.py 验证）。

### 14.7 关键参数

| 宏 | 值 | 说明 |
|----|----|----|
| `DATA_CHUNK_MAX` | 512 | DATA 帧数据上限（SD 扇区对齐）|
| `PAYLOAD_MAX` | 532 | 解析暂存缓冲（seq 4 + data 512 + 余量）|
| `USB_FNAME_MAX` | 64 | 文件名上限（FatFs LFN 已开）|
| RX 环形缓冲 | 2048 | ISR→主循环的字节环（停等下用不满）|

### 14.8 调试指南（按现象查）

MCU 端日志走 `gen_log`（USART1，tag `usb`）。先看串口，再看 PC 脚本输出：

| 现象 | 可能原因 | 查哪里 |
|------|---------|--------|
| `BEGIN status=0xF1 OPENFAIL` | 无 SD 卡 / FATFS 未挂载 | 插卡；看 boot 日志 `init: FATFS PASS` |
| PC 收不到任何 ACK（超时）| MCU 没在收 / 帧没解析 | 串口有无 `usb: BEGIN ...`；`CDC_Receive_FS` 是否接上 `app_usb_rx_push` |
| `DATA status=0xDEAD0001`(FR_DISK_ERR) | SDIO 写 FIFO 欠载 | HARDWARE.md「SDIO」降时钟；考虑 SDIO DMA |
| `EOF status=0xF4` CRC 不符 | 字节丢失/错序 | 串口 `got=N/size`、`crc=A/B`；看 `rx dropped` 是否非 0 |
| `rx ring dropped K bytes`(串口) | 环溢出（不该发生）| 停等被破坏？PC 是否在 ACK 前连发？ |

> Phase 4 的 PC GUI worker（`opfp_transfer.py`）将**原样复用** `test_cdc_file.py` 的成帧/解析逻辑
> （`frame()` / `recv_frame()`），仅加 QThread + 进度信号。修改协议时**两端同步改**：
> `app_usb.c` 宏 + `test_cdc_file.py`（及后续 `opfp_transfer.py`）。

---

## 15. .opfp v5 格式：编程配置段（PGCF，2026-08-29）

> GUI v5（产线四区布局）引入。**v5 = v4 + 24B 编程配置段**，紧跟 16B 显示名之后。

### 15.1 段结构（偏移相对文件头 128 起，28B）

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 128+0 | magic | 4B | `0x50474346`（"PGCF"） |
| 128+4 | flags | 4B | bit0=滚码 bit1=次数上限 bit2=加密占位 bit3=镜像编号已设置 |
| 128+8 | serial_addr | 4B | 滚码写入的 flash 绝对地址（如 `0x0800FC00`） |
| 128+12 | serial_width | 1B | 1/2/4 字节 |
| 128+13 | serial_step | 1B | 步进（默认 1） |
| 128+14 | reserved | 2B | 0 |
| 128+16 | serial_start | 4B | 起始序列号（第 1 次烧写 start，第 2 次 start+step…） |
| 128+20 | max_burn_count | 4B | 最大烧录次数（0=不限；MCU 按已持久化计数比对拒绝） |
| 128+24 | image_id | 4B | 镜像编号 1~9999（0=未设置；烧录器列表行首显示，多文件快速定位） |

- 文件总布局：`112B header + 16B 显示名 + 28B PGCF + algo blob + firmware`
- **段长判别**：初版 v5 段为 24B（无 image_id）。导入解析用 header 自带
  `fw_size + algo_size` 反推总长唯一确定段长（`pgcf_len_of()`）——不能用 len 阈值
  判（algo+fw 较长时旧文件同样超阈值，判错导致 blob 错位）
- header 仍是 v3 布局（112B），`version=5`；CRC 照旧只覆盖 112B（PGCF/名称不参与）
- **MCU（V1.8.0）**：v5 文件进列表 + 行首「NNN」编号块（青底白字）+ 烧录页名称前缀；
  PGCF magic 坏拒烧（码 11）。滚码执行/次数比对拒烧为后续版本

### 15.2 语义（产线）

- **滚码（递增序列号）**：MCU 每次烧录烧完固件后，把 `当前序列号` 写到 `serial_addr`（宽度
  `serial_width`），下次烧录 `+step`。序列号游标可复用 V1.7.0 的 Sector4 计数存储（同 hash 追加）。
- **编程次数上限**：`.opfp` 携带 `max_burn_count`，MCU 烧前比对已有 burn_count（持久化），
  达上限拒绝（错误码待定）。防外包产线超产。
- **编程加密**：占位。flags bit2 置位但本轮 PC 不加密落盘；算法（倾向 AES-128）与 MCU
  端实现后续里程碑。

### 15.3 GUI v5 布局

四区（2×2）：①芯片信息（厂家→型号两级下拉，可输入过滤；导入固件后实时显示
`FLASH 占用 x/总容量 (%)` + `FLASH 校验和 CRC32`）②编程配置（滚码/次数/加密三组）
③文件导入导出（导入原始固件 / 导出 .opfp / **导入 .opfp 回填**——改配置再导出）④操作日志
（时间戳 + 着色：绿=成功 红=失败 黄=警示，QPlainTextEdit 限 2000 行）。

导入回填的型号反查按「flash 三参数 + algo blob 字节级」匹配；多型号完全同参（如
STM32F401RB/F401CB 同 flash 同算法）时弹窗让用户确认——`.opfp` 不含型号字符串，无法唯一还原。

### 15.4 验证

`tests/test_gui_v5.py`（offscreen，无 pytest-qt 依赖，`python tests/test_gui_v5.py`）：
31 项断言——构造/选型/占用与校验和显示/v5 字节布局（PGCF 偏移、flags、总长）/滚码越界与
未对齐拒绝/导入回填往返/v4 无段兼容。

### 15.5 auth_nonce 授权号（V1.10.0，段 32B）

**问题**：计数按文件名持久化 → 文件名相同的"新授权"继承旧计数；SD 拷出重导入
即可洗次数（限制次数有漏洞）。

**机制**：
- PC 每次「导出/发送」生成随机 `auth_nonce`（4B，os.urandom，0 保留）写入 PGCF
- 设备端计数键 = `#N<nonce>`（旧 28B 段无 nonce → 回退文件名）——**同一字节文件
  （SD 拷贝/USB 重传/重导入）计数延续；只有 PC 重新导出（新 nonce）才从 0 计**
- **授权高水位**：按 image_id 存 `#HW<NNN>` = 见过的最大 nonce；旧 nonce 文件拒烧
  （码 15）——防把作废授权的旧文件拷回 SD 回灌
- 滚码游标键 = `#S<serial_addr>`（跨授权延续，序列号永不回绕重号）
- 设备永不导出文件（USB 协议只写不读——立为不变量）

**已知边界**（单机方案的极限，需后续里程碑补）：
- 拿到 PC GUI 的任何人可自制新授权（nonce 随便填）——根治需 PGCF 签名/加密
  （AES-CMAC，密钥仅存授权 PC 与设备）
- 两台烧录器 = 双份额度（计数按设备隔离）——根治需在线激活/中央计数
- SD 卡文件可被读卡器复制，但**复制体 nonce 相同 → 计数延续**，复制无用

### 15.6 v6 烧录账本（V1.11.0 安全闭环 → V1.12.0 计数迁回设备 flash）

> **V1.12.0 模型变更**：UID 已把文件锁死单设备，计数**不再随文件走**——权威源
> = 设备内部 flash（`app_count`，nonce 键），烧录后**不写 SD**（速度/寿命无忧，
> SD 损坏换卡重导计数不丢）。文件尾 56B 段退化为**静态绑定证书**（uid+nonce+
> CMAC 盖 PGCF），导入时绑一次 uid，之后永不修改；burned/seq 字段保留为摆设
> （PC 照写，设备忽略）。身份与计数的键 = auth_nonce——文件改名/换编号重导
> 入，同 nonce 即同账户，次数保持、洗不掉。

**文件布局**：`header 112 + 名 16 + PGCF 32 + algo + fw + 账本 56`
（账本 = iv 8 + ct 32 + cmac 16；明文 32B = burned/seq/nonce/rsv/uid[12]/fw_crc）。

**机制**：
- 计数随文件走（拷贝/换卡/换设备计数延续）—— AES-CTR 加密（雪崩效应，
  比对前后无法定位计数字节）+ AES-CMAC 认证（改任何比特拒烧，防位翻转）
- MCU UID 绑定（V1.11.4 起导入即绑）：USB 传输完成时设备验账本——未绑定→
  绑本机 UID 重封写回（文件落 SD 即锁设备）；已绑他机→删文件整传输拒
  （STAT 0xF6，PC 端不重试直接报「请在本机重新导出」）。SD 直拷不经 USB，
  仍在烧时拒（码 16，屏显 OTHER-DEV!）
- 单调流水号 seq + 设备端高水位（app_count `#L<nonce>`）：回滚旧账本备份
  → seq 落后拒烧（码 18）
- fw_crc 绑定：防「账本完好但固件体被换」的拼接攻击
- 主密钥：设备 Sector5 基部 0x08020000（"KEY1"+16B），PC 端 secret.key
  （.gitignore 排除）。工作密钥 = AES(主密钥, nonce_be||0^12)——每文件独立，
  泄漏一个不伤全局
- PC 无 secret.key 时降级生成 v5（无账本，日志警示）

**密码一致性**：MCU app_sec.c 与 PC opfp_sec.py 逐比特对拍通过
（AES-FIPS197 / CMAC-RFC4493 官方向量 + 三组交叉向量）。

**部署流程**（V1.11.2 起：仅源码注入，一次烧录）：
1. `python gen_key.py` 生成 secret.key（PC 端）+ app_sec_key.h.txt（C 片段）
2. 把片段里的 16 字节抄进 app/app_sec.c 的 SEC_KEY_SRC 宏（该改动不入 git）
3. Keil 编译烧固件——密钥随固件进设备，一次烧录完成
   （V1.11.0/1 曾支持 JLink 烧 key_img.bin @0x08020000 的方式，V1.11.2 已删除）

**残余边界**（如实告知）：拆机 dump 设备 flash 得主密钥可伪造账本；PC 端
secret.key 泄漏同理。根治需安全芯片（密钥不出芯片）或在线授权。


### 15.7 v7 固件段强制加密（V1.13.0）

**问题**：.opfp 固件段是明文——从 SD 卡拷出文件即可提取原始 .bin（实测验证：
`opfp[fw_off:fw_off+fw_size]` 与原 .bin 逐字节一致），用 JLink 直烧绕过一切限制。

**方案**（无开关，基础安全）：
- 固件段 AES-CTR 加密：K_fw = AES(主密钥, nonce_be||"FW"||0^10)（域分隔于
  账本密钥），IV = nonce_be||1_be，长度不变（CTR 无填充）
- 文件布局 v7：`header(112)+名(16)+PGCF(32)+algo(明文)+固件(密文)+证书(56)`
- 证书 fw_crc 字段 = 固件**密文** CRC32——烧录流式累计密文 CRC 终检（防密文
  被替换后解密出"合法但错误"的内容）
- MCU 烧录/回读验证均流式解密（2KB 块，块号跨块续算；MCU 与 PC 逐比特对拍）
- **MCU 拒 v6 明文固件文件**（码 4）——存量文件须 PC 重新导出
- PC 无 secret.key → 降级生成 v6 + 红字警告（开发调试用）
- GUI「编程加密」占位开关删除（强制启用，状态标签显示加密结果）

**边界**（如实告知）：主密钥泄漏（PC secret.key 或设备固件逆向）即可解密
一切文件——密钥保管是前提；拆机逆向固件提取密钥超出软件方案能力。


### 15.8 V2.0.0 加密下沉设备端（替代 §15.7 的 PC 加密路线）

**架构**：PC 工具零密码学（明文 .opfp v6：固件明文 + PGCF nonce，无证书无
secret.key 依赖）→ 厂家受控 USB 导入 → 烧录器接收完成后整文件加密落盘。

**SD 密文布局**：`[ENC1 头 16B（magic+nonce+保留）][AES-CTR 密文=原明文全文]
[CMAC 16B]`；`K_dev = AES(DEV_SALT, UID||0^4)`（app_sec.c 编译期 SALT +
设备唯一 UID）；IV = nonce||"EN"，块号 1 起 LE。

**安全性质**：
- 使用者拿 SD 密文拷贝/换设备均无效（UID 派生密钥不同）
- 改 SD 密文任何字节 → CMAC 拒（码 17）
- 裸明文 .opfp 直拷进 SD → 无 ENC1 头拒烧（码 4）
- 反编译 PC 工具无所得（无密钥无算法）；使用者无原始固件 → 无法自造有
  价值的文件
- USB 传输为明文——威胁模型内（导入=厂家开发人员受控操作），非漏洞

**部署**：DEV_SALT 配置在 app_sec.c（16B 随机，gen_key.py 可产出片段）；
换 SALT = 所有已落盘密文作废。边界（如实告知）：拆机逆向固件可得 SALT，
读 UID 可解本台 SD——与 PC 加密路线相同，防拆需安全芯片。


---

## 16. 安全体系档案（V1.9.0 ~ V2.1.2，独立章节）

> 本项目安全功能历经多轮方案迭代与真机攻防验证。本章完整记录：发现的漏洞、
> 评估过的方案、最终选型与理由。先后顺序即演进时间线。

### 16.1 已识别的安全漏洞（威胁模型：使用者=不可信，持有烧录器+PC 软件）

| # | 漏洞 | 攻击方式 | 后果 |
|---|------|---------|------|
| V1 | 固件明文存放 | 从 SD 卡拷出 .opfp，按偏移切出固件段即得原始 .bin | JLink 直烧任意芯片，绕过全部限制；逆向产品固件 |
| V2 | 计数存设备 flash 按文件名/nonce | 换设备/重导入可重置计数 | 超产（外包产线多烧） |
| V3 | 明文策略字段可篡改 | 十六进制改 maxcnt/滚码参数 | 提高上限绕过限制 |
| V4 | 文件可跨设备使用 | SD 卡换设备插入 | 每台设备各烧满额度（额度×设备数） |
| V5 | 计数随文件回滚 | 备份低计数时的文件副本，拷回 | 洗掉计数无限烧 |
| V6 | PC 软件持有密钥 | 反编译 PC 工具提取密钥/算法，自造合法文件 | 整个体系伪造 |

### 16.2 方案演进与最终选型

**① 计数防护（V1.10~V1.12 演进三次）**
- A. nonce 授权号随文件 + 设备端高水位拒旧 nonce（V1.10）→ 洗计数被堵
- B. 加密账本随文件走（AES-CMAC 计数 + UID 绑定 + 流水号防回滚，V1.11）
  → 堵 V3/V4/V5，但 SD 损坏换卡会被误拒、每次烧录写 SD
- C. **最终选：计数回设备 flash（nonce 键）+ UID 锁设备（V1.12）**
  - 理由：UID 绑定后文件已无法跨设备，计数随文件的原动机（防拷贝洗次数）消失
  - 收益：SD 零写入（寿命/速度无忧）、回滚免疫（回滚无收益）、换卡重导计数不丢
  - 残余：PC 重新导出=新 nonce=新计数（授权语义，非漏洞）

**② 固件加密（V1.13 → V2.0 → V2.1 演进三次）**
- A. PC 端 AES-CTR 加密固件段（v7，V1.13）→ 堵 V1，但 PC 持密钥（V6 暴露）
- B. 用户提出：PC 生成明文，**设备接收后加密落盘**——评估发现比 A 优：
  - PC 零密码学，反编译无所得（V6 消除）
  - 使用者无原始固件，无法自造有价值的 .opfp（自造文件烧的是自己的程序，无意义）
  - USB 明文传输在威胁模型内（导入=厂家开发人员受控操作），非漏洞
- C. **最终选：方案 B + 密钥 = f(用户口令, MCU UID)（V2.1）**
  - K0=口令循环填充^位置 → K1=AES(K0,K0) → K_dev=AES(K1,UID||0^4)
  - 口令持久化设备 Sector5（跨重启一致），换口令=旧密文作废
  - SD 落盘格式：[ENC1 头 16B][AES-CTR 密文][CMAC 16B]；改任何字节拒烧

**③ 防篡改（贯穿）**
- header CRC32（V1.6）→ PGCF 明文字段被改的漏洞（V1.11.3 发现）→
  CMAC 扩盖策略段 → V2.1 整文件 CMAC（ENC 层）一揽子解决

### 16.3 真机攻防验证记录

| 验证 | 结果 |
|------|------|
| 从 SD 拷出文件提取固件 | ❌ 无法（密文+UID 绑定，PC 无密钥） |
| 十六进制改密文任意字节 | ❌ CMAC 拒（ERR 17） |
| 明文 .opfp 直接拷进 SD | ❌ 无 ENC1 头拒烧（ERR 4） |
| 换口令后旧密文 | ❌ CMAC 拒（预期：换口令=旧文件作废） |
| 跨设备（不同 UID） | ❌ 密钥派生不同，无法解密 |
| 比对明密文对推密钥 | ❌ AES-128 已知明文攻击无效 + 每文件 nonce 独立密钥流 |

### 16.4 排障实录（V2.0/V2.1 联调期，含根因与修复）

1. **栈溢出致 EOF 后复位**：加密函数 2048B 栈缓冲+FIL 叠在 USB 调用链上撞
   4KB 栈底 → buf 改 static。教训：大缓冲一律 static/全局
2. **FatFs LFN 堆耗尽**：_USE_LFN=2 每路径 512B 堆，CubeMX 默认堆 512B 仅容
   一个路径 → 堆扩 4KB。教训：用堆的中间件先算配额
3. **CMAC 两份"等价"内联副本算出不同结果**：加密端/验证端各自实现同一段
   算法，实测分歧 → 重构为 app_sec 共享流式实现（begin/update/end）。
   教训：**同一算法绝不写两份**
4. **重构误删写 ENC1 头代码**：锚点区间替换吞掉了区间内的写头段 → 文件头
   残留明文前 16B，验证端误走明文路径，16B 后字段全乱。
   教训：区间替换必须枚举边界内全部代码
5. **f_tell 在加密读路径返回异常值**：按格式显式计算偏移替代 f_tell
6. **裸 printf 无调试器挂死**：半主机模式等调试器 → 必须 bsp_uart_log
7. **flash 地址裸 swd_write 被静默丢弃**（滚码 0xFFFFFFFF）：flash 写必须走
   algo 的 program_page syscall；且目标页须先擦（擦除范围扩到滚码页）

### 16.5 残余边界（如实告知，软件方案极限）

- 拆机 SWD dump 固件可提取口令推导逻辑与 UID → 可解本台密文。防拆需
  安全芯片（密钥不出芯片）或 RDP Level 2（不可逆）
- 两台设备=双份额度（计数按设备隔离）。根治需在线授权服务器
- 加密口令明文过 USB 线：威胁模型内（厂家受控导入），非漏洞

### 16.6 现行安全参数速查

| 项 | 值 |
|---|---|
| 密钥 | K_dev = AES(AES(K0,K0), UID)，K0=口令混淆填充 |
| 默认口令 | "DONECHIP"（PC 口令框留空时） |
| 加密 | AES-128-CTR，IV=nonce||"EN"，块号 1 起 LE |
| 认证 | AES-CMAC(RFC4493) 覆盖全部密文 |
| 口令存储 | 设备内部 flash Sector5（0x08020000），跨重启恢复 |
| 文件格式 | [ENC1+nonce 16B][密文][CMAC 16B] |
| PC 端密码学 | 无（零密钥零算法） |
