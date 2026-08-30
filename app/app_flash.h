#ifndef __APP_FLASH_H
#define __APP_FLASH_H

/**
 * @file app_flash.h
 * @brief 脱机烧录：从 SD 读 .opfp → 头校验 → 烧录 → 回读验证（可靠性闭环）
 *
 * app_flash_run2() 完成一次烧录：读 .opfp → header CRC32 校验（坏文件拒烧）→
 * SWD 连接 → 擦除 + 编程 → 回读 flash 逐块比对（在 目标重新上锁前）。
 * 编程失败(9)/校验不一致(12)自动整流程重试一次。
 * 返回码：0=成功 1=无文件 2=open 3=read-hdr 4=magic 5=algo超大 6=algo读
 *         7=connect/begin 8=固件读 9=编程 10=越界 11=头CRC错 12=回读不一致
 */

#include <stdint.h>

/* 烧录进度回调：percent 0~100。回调里可驱动 UI 进度条（回调频繁，勿做重活） */
typedef void (*app_flash_progress_cb_t)(uint8_t percent);

/**
 * @brief 执行一次脱机烧录（指定文件 + 进度回调 + 回读验证）
 *   path     : "0:xxx.opfp"（app_ui 扫描选定）；NULL = 根目录自动找第一个
 *   progress : 进度回调（可为 NULL）；0~95 编程，96~99 回读验证，100 完成
 *   返回 0=成功, 非0=失败码（见文件头注释；11=文件坏，12=烧进去了但读回不一致）
 */
int app_flash_run2(const char *path, app_flash_progress_cb_t progress);

/* 兼容旧接口（无进度）：等价于 app_flash_run2(NULL, NULL) */
int app_flash_run(void);

/** 选中 .opfp 的参数快照（烧录页显示用） */
typedef struct {
    char     filename[40];      /* "xxx.opfp"（不含路径） */
    char     disp_name[17];     /* v4 显示名（≤16 可打印 ASCII；空=未设置，UI 回退显示文件名） */
    char     chip_model[24];    /* 文件名反推：-<型号>.opfp */
    uint32_t fw_size;
    uint32_t flash_start;
    uint32_t flash_size;
    uint32_t burn_count;        /* 该文件累计烧录次数（app_count 持久化，parse 时查） */
    uint32_t image_id;          /* v5 镜像编号（0=未设置；列表/烧录页显示用） */
    uint32_t max_burn_count;    /* v5 次数上限（0=不限；烧录页显示） */
    uint32_t serial_addr;       /* v5 滚码地址（0=未启用） */
    uint32_t serial_start;      /* v5 滚码起始值 */
    uint8_t  serial_width;      /* v5 滚码宽度 1/2/4 */
    uint8_t  serial_on;         /* v5 滚码启用 */
    uint8_t  maxcnt_on;         /* v5 次数上限启用 */
    uint8_t  serial_step;       /* v5 滚码步进（显示/游标推进用） */
    uint32_t auth_nonce;        /* v5 授权号（计数键用；0=旧段回退文件名） */
    uint32_t ledger_seq;        /* v6 证书流水号（V2.0 已忽略，字段保留） */
    uint8_t  ledger_ok;         /* v6 证书验过（V2.0 下恒 0，字段保留） */
    uint8_t  ledger_err;        /* 证书错误码（16=UID 17=坏；V2.0 由 ENC 层承担） */
    uint8_t  rdp;               /* rdp_type != 0 */
} app_flash_info_t;

/**
 * @brief 烧录页待机时预检：已达次数上限（maxcnt 启用且 burn_count>=max）
 */
int app_flash_count_exceeded(const app_flash_info_t *info);

/** 最近一次 parse/run2 的计数持久化键（UI 刷新计数用） */
const char *app_flash_count_key(void);

/**
 * @brief 解析 .opfp header 填 info（不烧录，P2 参数页用）
 *   返回 0=成功
 */
int app_flash_parse(const char *path, app_flash_info_t *info);

#endif /* __APP_FLASH_H */
