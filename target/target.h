#ifndef __TARGET_H
#define __TARGET_H

#include <stdint.h>
#include "error.h"
#include "flash_blob.h"
#include "app_rdp.h"       /* rdp_params_t */

/**
 * @file target.h
 * @brief 目标芯片抽象 + 传输层接口 + 会话
 * Phase 3+: RDP 参数也来自文件（方案 B 数据化）
 */

/* ---- 传输层接口 ---- */
typedef struct transport_ops {
    uint8_t (*connect)   (void);
    uint8_t (*disconnect)(void);
    uint8_t (*write_mem) (uint32_t addr, const uint8_t *data, uint32_t size);
    uint8_t (*read_mem)  (uint32_t addr, uint8_t *data, uint32_t size);
} transport_ops_t;

/* ---- 目标芯片对象 ---- */
typedef struct target_chip {
    const char       *name;
    uint32_t          flash_start;
    uint32_t          flash_size;   /* flash 总大小（v3，越界校验用）*/
    uint32_t          page_size;
    const program_target_t *algo;
    rdp_params_t      rdp;          /* RDP 参数（从 .opfp 填充）*/
} target_chip_t;

/* ---- 会话 ---- */
typedef struct {
    const target_chip_t     *chip;
    const transport_ops_t   *ops;
} target_session_t;

/* ---- 通用烧录流程 ---- */
error_t target_program_begin(target_session_t *s, uint32_t fw_size);
error_t target_program_page(target_session_t *s, const uint8_t *data, uint32_t len);
void target_program_end(target_session_t *s);

extern const transport_ops_t swd_ops;

#endif /* __TARGET_H */
