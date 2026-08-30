#include <stdint.h>

#ifndef __APP_RDP_H
#define __APP_RDP_H

/**
 * @file app_rdp.h
 * @brief 目标读保护(RDP)处理 — MCU 内置参数（方案 B）
 *
 * RDP 参数固化在 MCU（app_rdp.c 的 RDP_TABLE，按 rdp_type）。
 * .opfp 只传 rdp_type（族标识，PC 端 build 按芯片族自动标）。
 * 用户无需手动选「启用读保护」—— MCU 自动检测，有保护才解锁（mass-erase）。
 * （开源版：接口保留，解锁实现未包含，见 app_rdp.c 说明）
 *
 * rdp_params_t 仅 type 字段被使用（从 .opfp 的 rdp_type 填充）；
 * 其余字段保留兼容旧 .opfp，但 MCU 内置表优先，不再读取。
 */

/* RDP 参数（仅 type 由 MCU 使用，其余保留兼容）*/
typedef struct {
    uint16_t type;          /* 0=none, 1=F1式, 2=F4式 */
    uint32_t fpec_base;     /* 保留（MCU 内置，不用）*/
    uint32_t key1, key2;    /* 保留 */
    uint32_t optkey1, optkey2;  /* 保留 */
    uint32_t ob_addr;       /* 保留 */
    uint32_t ob_val;        /* 保留 */
    uint32_t sr_bsy_mask;   /* 保留 */
} rdp_params_t;

/* 检查目标是否设了读保护。返回 1=已保护, 0=未保护 */
int app_rdp_is_protected(const rdp_params_t *p);

/* 解除读保护（会整片擦除目标）。返回 0=成功, 非0=失败码 */
int app_rdp_unlock(const rdp_params_t *p);

#endif /* __APP_RDP_H */
