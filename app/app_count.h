#ifndef __APP_COUNT_H
#define __APP_COUNT_H

#include <stdint.h>

/**
 * @file app_count.h
 * @brief 烧录计数持久化（每个 .opfp 文件一份累计次数）
 *
 * 存储：STM32 内部 flash Sector4 顶部（0x0801F000~0x0801FFFF，4KB）。
 * F401RB 布局：Sector0-3 = 4x16KB（固件 57KB 只用到 ~0x0800F000），
 * Sector4 = 64KB 完全空闲 —— 固件与参数区隔 Sector 边界，互不干扰。
 *
 * 记录模型（append-only 磨损均衡）：
 *   4KB = 256 条记录槽（16B/条）。烧录成功后追加写一条 {magic, name_hash, count}；
 *   查询 = 从头扫到第一条空槽（0xFF），取该 hash 的最后一条。
 *   槽写满后整区擦除重写（把每个 hash 的最新值压缩到前面）→ 擦写次数 =
 *   烧录次数/256，Sector4 10K 次寿命 → 支撑 ~250 万次烧录。
 *
 * 掉电安全：追加写在计数已 +1 的数据区，写一半掉电只会丢最后一条
 * （下次读到旧值，最多少计 1 次），不会损坏历史。
 */

/* 查询指定文件的累计烧录次数（按文件名散列）。无记录返回 0。 */
uint32_t app_count_get(const char *filename);

/* 烧录成功后调用：该文件计数 +1 并持久化。返回 0=成功。 */
int app_count_inc(const char *filename);

/* 授权高水位用：把该键的值写为指定值（仅当大于现有值，单调不回退）。返回 0=成功。 */
int app_count_inc_raw(const char *key, uint32_t value);

#endif /* __APP_COUNT_H */
